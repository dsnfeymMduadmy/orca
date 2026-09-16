#include "orca_usb_msc.h"
#include "orca_memmap.h"
#include "orca_ui.h"
#include "bsp_storage.h"
#include "main.h"
#include "usbd_core.h"
#include "usbd_msc.h"
#include "usbd_msc_scsi.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>

/*
 * Класс Mass Storage поверх апстримного стека ST. Здесь только то, чего
 * апстрим знать не может: какие у платы носители, кто ими сейчас владеет и
 * что отвечать хосту на INQUIRY. Пины, тактирование и мост USBD_LL_* лежат
 * в Core/Src/bsp_usb.c, дескрипторы устройства — в Core/Src/usbd_desc.c,
 * общая картина — в orca_usb_msc.h.
 */

extern USBD_DescriptorsTypeDef orca_usb_descriptors;
extern bool bsp_usb_irq_start(void);

/*
 * Опрос шины. 10 мс, а не 100: между SET_CONFIGURATION и первым чтением
 * таблицы разделов проходят единицы миллисекунд, и всё это время тома ещё
 * смонтированы ядром. Чем короче окно, тем меньше шансов, что хост прочитает
 * FAT, которую ядро в этот момент дописывает.
 */
#define MSC_POLL_MS         10u

/*
 * Пауза в записи, после которой кеш сектора QSPI сбрасывается в чип.
 * Хост считает данные записанными сразу после ответа на WRITE(10), а у нас
 * последний сектор до сброса лежит в RAM (см. bsp_qspi.c). 200 мс — заметно
 * больше паузы внутри одной операции копирования и заметно меньше времени,
 * за которое человек успевает выдернуть кабель.
 */
#define MSC_FLUSH_IDLE_MS   200u

/*
 * Сколько колбэк SCSI ждёт носитель, занятый ядром. Хост терпит команду
 * секунды (у Linux таймаут SCSI 30 с), а дольше всех держит носитель
 * f_mkfs чистого QSPI — секунды. Ждать без ограничения нельзя: лучше
 * ответить хосту отказом, чем оставить задачу USB висеть навсегда.
 */
#define MSC_LOCK_WAIT_MS    3000u

static USBD_HandleTypeDef s_dev;
static bool               s_up;

/*
 * Носитель по очереди трогают две задачи: usbd (колбэки SCSI) и usb
 * (монтирование, сброс кеша). У QSPI один кеш сектора на всех, стирание
 * длится десятки миллисекунд, и вторая задача, влезшая в середину, оставила
 * бы в чипе половину старых данных.
 */
static SemaphoreHandle_t s_lock;

/*
 * true — носители переданы хосту (тома FatFs размонтированы). Пишет задача
 * usb, читает задача usbd, поэтому volatile.
 */
static volatile bool     s_host_owns;
static volatile bool     s_write_pending;
static volatile uint32_t s_write_tick;
static volatile uint32_t s_blocks_read;
static volatile uint32_t s_blocks_written;

/* ------------------------------------------------------------------ */
/* Ответ на INQUIRY                                                    */
/* ------------------------------------------------------------------ */

/*
 * Строки INQUIRY — это то, что хост показывает как модель диска (lsblk
 * MODEL, «Свойства устройства» в Windows). Собираются в рантайме, а не
 * пишутся массивом байт: в SCSI поля фиксированной длины и добиваются
 * пробелами, и такой массив невозможно поправить, не пересчитав вручную
 * смещения.
 */
static int8_t s_inquiry[ORCA_MSC_LUN_COUNT * STANDARD_INQUIRY_DATA_LEN];

static void inquiry_fill(uint8_t lun, bool removable, const char* product)
{
    int8_t* p = &s_inquiry[lun * STANDARD_INQUIRY_DATA_LEN];
    size_t  n = strlen(product);

    memset(p, ' ', STANDARD_INQUIRY_DATA_LEN);

    p[0] = 0x00;                                   /* direct-access block device */
    p[1] = removable ? (int8_t)0x80 : 0x00;        /* RMB */
    p[2] = 0x02;                                   /* SPC-2 */
    p[3] = 0x02;                                   /* формат ответа */
    p[4] = (int8_t)(STANDARD_INQUIRY_DATA_LEN - 5u);
    p[5] = 0x00;
    p[6] = 0x00;
    p[7] = 0x00;

    memcpy(&p[8], "Orca    ", 8);                  /* vendor, ровно 8 байт */
    memcpy(&p[16], product, (n > 16u) ? 16u : n);  /* product, до 16 байт */
    memcpy(&p[32], "1.0 ", 4);                     /* revision */
}

/* ------------------------------------------------------------------ */
/* Носители                                                            */
/* ------------------------------------------------------------------ */

static uint32_t flash_blocks(void)
{
    return ORCA_FLASH_SIZE / BSP_BLOCK_SIZE;
}

/*
 * Внутренняя FLASH читается обычным memcpy: она отображена в адресное
 * пространство (0x08000000), и отдельный драйвер ей не нужен. Писать её
 * хост не может — st_write_protected() отдаёт LUN 0 только на чтение.
 */
static bool flash_read(uint8_t* buf, uint32_t lba, uint32_t count)
{
    if (lba + count > flash_blocks()) {
        return false;
    }
    memcpy(buf, (const void*)(ORCA_FLASH_BASE + lba * BSP_BLOCK_SIZE),
           (size_t)count * BSP_BLOCK_SIZE);
    return true;
}

/* ------------------------------------------------------------------ */
/* Колбэки хранилища для класса MSC                                    */
/* ------------------------------------------------------------------ */

/*
 * ВАЖНО: read/write вызываются стеком по ходу разбора BOT, то есть из задачи
 * usbd (см. OTG_FS_IRQHandler в Core/Src/bsp_usb.c — прерывание передаёт
 * разбор задаче именно ради этого). Блокирующие драйверы SD и QSPI здесь
 * законны, а вот FatFs трогать по-прежнему нельзя: тома в это время либо
 * размонтированы, либо принадлежат ядру.
 */

static bool lock_take(void)
{
    if (s_lock == NULL) {
        return false;
    }
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(MSC_LOCK_WAIT_MS)) == pdTRUE;
}

static void lock_give(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static int8_t st_init(uint8_t lun)
{
    (void)lun;
    return 0;
}

static int8_t st_capacity(uint8_t lun, uint32_t* block_num, uint16_t* block_size)
{
    *block_size = (uint16_t)BSP_BLOCK_SIZE;

    switch (lun) {
        case ORCA_MSC_LUN_FLASH: *block_num = flash_blocks();          break;
        case ORCA_MSC_LUN_QSPI:  *block_num = bsp_qspi_block_count();  break;
        case ORCA_MSC_LUN_SD:    *block_num = bsp_sdmmc_block_count(); break;
        default:                 *block_num = 0; return -1;
    }

    /* Нулевая ёмкость для хоста — «носителя нет», а не «диск на 0 байт». */
    return (*block_num != 0u) ? 0 : -1;
}

static int8_t st_ready(uint8_t lun)
{
    switch (lun) {
        case ORCA_MSC_LUN_FLASH: return 0;
        case ORCA_MSC_LUN_QSPI:  return bsp_qspi_ready()  ? 0 : -1;
        case ORCA_MSC_LUN_SD:    return bsp_sdmmc_ready() ? 0 : -1;
        default:                 return -1;
    }
}

/*
 * Признак «только чтение» отдаётся честно один раз на монтирование: хост
 * читает его в MODE SENSE и, увидев защиту, монтирует том read-only до
 * следующего подключения. Поэтому переходное состояние (тома ещё у ядра)
 * здесь не отражается — им занимается st_write.
 */
static int8_t st_write_protected(uint8_t lun)
{
    return (lun == ORCA_MSC_LUN_FLASH) ? 1 : 0;
}

static int8_t st_read(uint8_t lun, uint8_t* buf, uint32_t blk_addr, uint16_t blk_len)
{
    bool ok;

    if (lun == ORCA_MSC_LUN_FLASH) {
        /* Внутренняя FLASH — это memcpy из адресного пространства; ни с кем
         * не делится и никого не ждёт. */
        return flash_read(buf, blk_addr, blk_len) ? 0 : -1;
    }

    if (!lock_take()) {
        return -1;
    }
    switch (lun) {
        case ORCA_MSC_LUN_QSPI:  ok = bsp_qspi_block_read(buf, blk_addr, blk_len);   break;
        case ORCA_MSC_LUN_SD:    ok = bsp_sdmmc_block_read(buf, blk_addr, blk_len);  break;
        default:                 ok = false;                                        break;
    }
    lock_give();

    if (ok) {
        s_blocks_read += blk_len;
    }
    return ok ? 0 : -1;
}

static int8_t st_write(uint8_t lun, uint8_t* buf, uint32_t blk_addr, uint16_t blk_len)
{
    bool ok;

    /*
     * Пока ядро не отпустило тома, запись хоста отклоняется: у FatFs в этот
     * момент свой взгляд на FAT, и запись «в две руки» разошлась бы с ним.
     * Окно короткое (см. MSC_POLL_MS) и приходится на опрос диска хостом, а
     * не на копирование файлов, так что до отказа дело обычно не доходит.
     */
    if (!s_host_owns) {
        return -1;
    }
    if (!lock_take()) {
        return -1;
    }

    switch (lun) {
        case ORCA_MSC_LUN_QSPI:  ok = bsp_qspi_block_write(buf, blk_addr, blk_len);  break;
        case ORCA_MSC_LUN_SD:    ok = bsp_sdmmc_block_write(buf, blk_addr, blk_len); break;
        default:                 ok = false;                                         break;
    }
    lock_give();

    if (ok) {
        s_blocks_written += blk_len;
        s_write_tick     = (uint32_t)xTaskGetTickCount();
        s_write_pending  = true;
    }
    return ok ? 0 : -1;
}

static int8_t st_max_lun(void)
{
    return (int8_t)(ORCA_MSC_LUN_COUNT - 1u);
}

static USBD_StorageTypeDef s_storage = {
    st_init,
    st_capacity,
    st_ready,
    st_write_protected,
    st_read,
    st_write,
    st_max_lun,
    s_inquiry,
};

/* ------------------------------------------------------------------ */
/* Память под класс                                                    */
/* ------------------------------------------------------------------ */

/*
 * Апстрим выделяет данные класса через USBD_malloc (см. Core/Inc/usbd_conf.h).
 * Класс у нас один и создаётся один раз, поэтому «куча» — это один статический
 * блок: динамической памяти в ядре нет, а падать при нехватке некуда.
 */
void* USBD_static_malloc(uint32_t size)
{
    static uint32_t s_class_mem[(sizeof(USBD_MSC_BOT_HandleTypeDef) + 3u) / 4u];

    return (size <= sizeof(s_class_mem)) ? (void*)s_class_mem : NULL;
}

void USBD_static_free(void* p)
{
    (void)p;
}

/* ------------------------------------------------------------------ */
/* Передача носителей между ядром и хостом                             */
/* ------------------------------------------------------------------ */

/*
 * Сброс кеша QSPI под мьютексом: кеш сектора один, и писать в него, пока то
 * же самое делает задача usbd, нельзя — стирание сектора длится десятки
 * миллисекунд, и прилетевший в середине WRITE(10) оставил бы в чипе половину
 * старых данных. Хост от паузы не страдает: неотвеченные пакеты он получает
 * с NAK и повторяет их.
 */
static bool qspi_sync_locked(void)
{
    bool ok;

    if (!lock_take()) {
        return false;
    }
    ok = bsp_qspi_sync();
    lock_give();

    return ok;
}

/*
 * Хост считается владельцем носителей только в состоянии CONFIGURED: до него
 * класс не инициализирован и SCSI-команды не придут. Выдернутый кабель
 * приходит сюда как SUSPENDED — датчика VBUS на плате нет, и «хост уснул» от
 * «кабеля нет» не отличить (см. bsp_usb.c).
 *
 * Второе условие — «безопасное извлечение» на стороне хоста: оно приходит
 * командой START STOP UNIT с LOEJ, после неё хост к томам больше не
 * обращается, а кабель обычно остаётся в разъёме. Без этой проверки тома
 * оставались бы у уже не заинтересованного хоста, а устройство — без своей ФС.
 */
static bool host_active(void)
{
    if (s_dev.dev_state != USBD_STATE_CONFIGURED) {
        return false;
    }

    const USBD_MSC_BOT_HandleTypeDef* msc =
        (const USBD_MSC_BOT_HandleTypeDef*)s_dev.pClassDataCmsit[s_dev.classId];

    if (msc == NULL) {
        return false;
    }
    return msc->scsi_medium_state != SCSI_MEDIUM_EJECTED;
}

static void media_to_host(void)
{
    /*
     * Флаг поднимается последним: он же разрешает запись из задачи usbd, и
     * будь он выставлен раньше, WRITE(10) успел бы лечь в том, который ядро
     * ещё держит смонтированным.
     */
    bool locked = lock_take();

    bsp_sdmmc_unmount();
    bsp_qspi_unmount();

    if (locked) {
        lock_give();
    }

    s_write_pending = false;
    s_host_owns     = true;
}

static void media_to_kernel(void)
{
    s_host_owns = false;

    bool locked = lock_take();

    /* Всё, что хост оставил в кеше сектора, дописываем до монтирования. */
    if (s_write_pending) {
        s_write_pending = false;
        bsp_qspi_sync();
    }
    bsp_sdmmc_sync();

    bsp_sdmmc_mount();
    bsp_qspi_mount();

    if (locked) {
        lock_give();
    }

    /*
     * Каталог 0:/apps мог измениться, пока карта была у хоста, — именно так
     * приложения на неё и попадают. Список в GUI собран по старому виду
     * каталога, и без сброса кэша новое приложение не появилось бы на
     * странице до перезагрузки.
     */
    orca_ui_apps_changed();
}

/* ------------------------------------------------------------------ */
/* Метки томов                                                         */
/* ------------------------------------------------------------------ */

/*
 * Хост подписывает смонтированный том его FAT-меткой, а не именем USB-
 * устройства: без метки карта появляется в системе как безымянный набор
 * цифр. Метку ставим только если её нет — переименовывать том, который
 * пользователь назвал сам, мы не вправе.
 */
static void label_default(const char* volume, const char* label)
{
    char current[16] = {0};
    char path[24];

    if (f_getlabel(volume, current, NULL) != FR_OK || current[0] != '\0') {
        return;
    }
    if (snprintf(path, sizeof(path), "%s%s", volume, label) > 0) {
        f_setlabel(path);
    }
}

/* ------------------------------------------------------------------ */
/* Публичный интерфейс                                                 */
/* ------------------------------------------------------------------ */

bool orca_usb_msc_init(void)
{
    inquiry_fill(ORCA_MSC_LUN_FLASH, false, "Kernel Flash");
    inquiry_fill(ORCA_MSC_LUN_QSPI,  false, "QSPI NOR");
    inquiry_fill(ORCA_MSC_LUN_SD,    true,  "microSD");

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return false;
    }

    /*
     * QSPI монтируется здесь, а не в board_qspi_init: пустой чип
     * форматируется как раз при монтировании, и хосту он должен достаться
     * готовым томом, а не 16 МБ, которые тот предложит отформатировать.
     * Отказ не фатален — LUN всё равно останется доступным как блочное
     * устройство, просто без файловой системы.
     */
    bsp_qspi_mount();

    label_default("0:", "ORCA SD");
    label_default("1:", "ORCA QSPI");

    /*
     * Задача разбора прерывания — до USBD_Start. Стек ST выполняет команды
     * SCSI в контексте вызова HAL_PCD_IRQHandler, а нашим драйверам носителей
     * нужен идущий SysTick (подробности в Core/Src/bsp_usb.c).
     */
    if (!bsp_usb_irq_start()) {
        return false;
    }

    if (USBD_Init(&s_dev, &orca_usb_descriptors, 0) != USBD_OK) {
        return false;
    }
    if (USBD_RegisterClass(&s_dev, USBD_MSC_CLASS) != USBD_OK) {
        return false;
    }
    if (USBD_MSC_RegisterStorage(&s_dev, &s_storage) != (uint8_t)USBD_OK) {
        return false;
    }
    if (USBD_Start(&s_dev) != USBD_OK) {
        return false;
    }

    s_up = true;
    return true;
}

void orca_usb_msc_task(void* arg)
{
    (void)arg;

    for (;;) {
        bool host = host_active();

        if (host && !s_host_owns) {
            media_to_host();
        } else if (!host && s_host_owns) {
            media_to_kernel();
        } else if (host && s_write_pending) {
            uint32_t idle = (uint32_t)xTaskGetTickCount() - s_write_tick;

            if (idle >= pdMS_TO_TICKS(MSC_FLUSH_IDLE_MS)) {
                s_write_pending = false;
                qspi_sync_locked();
                bsp_sdmmc_sync();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MSC_POLL_MS));
    }
}

orca_msc_state_t orca_usb_msc_state(void)
{
    if (!s_up) {
        return ORCA_MSC_OFF;
    }
    return s_host_owns ? ORCA_MSC_HOST : ORCA_MSC_IDLE;
}

const char* orca_usb_msc_state_str(void)
{
    switch (orca_usb_msc_state()) {
        case ORCA_MSC_HOST: return "host";
        case ORCA_MSC_IDLE: return "idle";
        default:            return "off";
    }
}

uint32_t orca_usb_msc_blocks_read(void)
{
    return s_blocks_read;
}

uint32_t orca_usb_msc_blocks_written(void)
{
    return s_blocks_written;
}

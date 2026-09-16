#include "main.h"
#include "bsp_storage.h"
#include "orca_memmap.h"
#include "ff.h"
#include <string.h>

/*
 * SPI NOR 16 MB на QUADSPI, memory-mapped @ 0x90000000.
 *
 * На плате встречаются разные чипы: Winbond W25Q128 (JEDEC EF 40 18) и
 * GigaDevice GD25Q128 (C8 40 18). Набор команд у них общий и стандартный
 * для SPI NOR, поэтому драйвер один на всех.
 *
 * Пины:
 *   CLK  PB2   (AF9,  QUADSPI_CLK)
 *   NCS  PB6   (AF10, QUADSPI_BK1_NCS)
 *   IO0  PF8   (AF10, QUADSPI_BK1_IO0)
 *   IO1  PF9   (AF10, QUADSPI_BK1_IO1)
 *   IO2  PF7   (AF9,  QUADSPI_BK1_IO2)
 *   IO3  PF6   (AF9,  QUADSPI_BK1_IO3)
 *
 * ВНИМАНИЕ: PB6 (NCS) физически совпадает с DCMI_D5 камеры, а PF6/PF7
 * с камерой не конфликтуют. Одновременно QSPI и DCMI на этой плате не
 * поднять — см. docs/hardware.md.
 *
 * Чтение идёт через memory-mapped режим (просто memcpy с 0x90000000), а
 * запись — только индирект-командами, и на время записи режим приходится
 * сбрасывать: контроллер не умеет то и другое одновременно.
 */

#define QSPI_FLASH_SIZE_LOG2  23u   /* 2^24 = 16 MB, поле FlashSize = log2(size)-1 */
#define QSPI_FLASH_CAPACITY_ID 0x18u /* третий байт JEDEC ID: 2^0x18 = 16 MB */
#define QSPI_PAGE_SIZE        256u
#define QSPI_SECTOR_SIZE      4096u  /* минимальная стираемая единица */
#define QSPI_BLOCK_SIZE       BSP_BLOCK_SIZE /* «сектор» в терминах FatFs и SCSI */
#define QSPI_BLOCKS_PER_SECTOR (QSPI_SECTOR_SIZE / QSPI_BLOCK_SIZE)

#define W25Q_CMD_READ_ID          0x9F
#define W25Q_CMD_WRITE_ENABLE     0x06
#define W25Q_CMD_READ_STATUS1     0x05
#define W25Q_CMD_READ_STATUS2     0x35
#define W25Q_CMD_WRITE_STATUS2    0x31
#define W25Q_CMD_PAGE_PROGRAM     0x02
#define W25Q_CMD_SECTOR_ERASE_4K  0x20
#define W25Q_CMD_FAST_READ_QUAD   0xEB   /* Quad I/O Fast Read */
#define W25Q_STATUS2_QE           0x02

/*
 * Таймауты стирания и записи взяты из datasheet как «максимум»: типичное
 * стирание сектора 45 мс, но по документации оно имеет право занять 400.
 */
#define QSPI_ERASE_TIMEOUT_MS     600u
#define QSPI_PROGRAM_TIMEOUT_MS   20u
#define QSPI_STATUS_TIMEOUT_MS    100u

static QSPI_HandleTypeDef hqspi;


/*
 * Причина отказа, а не только его факт: «QSPI init failed» одинаково выглядит
 * и когда чипа на плате нет, и когда перепутана распиновка, и когда чип чужой,
 * а лечится это тремя разными способами. Ответ на JEDEC ID держим рядом:
 * 00 00 00 или FF FF FF означают, что на линии данных не отвечает никто, то
 * есть искать надо железо, а не настройки контроллера.
 */
static const char* s_fail_stage = "";
static uint8_t     s_jedec_id[3];
static bool        s_mm_active;
static bool        s_ready;

const char* bsp_qspi_error(void)
{
    return s_fail_stage;
}

const uint8_t* bsp_qspi_jedec_id(void)
{
    return s_jedec_id;
}

static bool qspi_fail(const char* stage)
{
    s_fail_stage = stage;
    return false;
}

static void qspi_gpio_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    gpio.Pin       = GPIO_PIN_2;
    gpio.Alternate = GPIO_AF9_QUADSPI;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin       = GPIO_PIN_6;
    gpio.Alternate = GPIO_AF10_QUADSPI;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.Alternate = GPIO_AF10_QUADSPI;
    HAL_GPIO_Init(GPIOF, &gpio);

    gpio.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Alternate = GPIO_AF9_QUADSPI;
    HAL_GPIO_Init(GPIOF, &gpio);
}

static bool qspi_cmd_simple(uint8_t instruction)
{
    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction     = instruction;
    cmd.AddressMode     = QSPI_ADDRESS_NONE;
    cmd.DataMode        = QSPI_DATA_NONE;
    cmd.DummyCycles     = 0;
    cmd.SIOOMode        = QSPI_SIOO_INST_EVERY_CMD;
    return HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) == HAL_OK;
}

static bool qspi_read_reg(uint8_t instruction, uint8_t* value)
{
    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction     = instruction;
    cmd.AddressMode     = QSPI_ADDRESS_NONE;
    cmd.DataMode        = QSPI_DATA_1_LINE;
    cmd.NbData          = 1;
    cmd.DummyCycles     = 0;
    cmd.SIOOMode        = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return false;
    }
    return HAL_QSPI_Receive(&hqspi, value, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) == HAL_OK;
}

/*
 * Ожидание конца внутренней операции чипа. Крутится по таймеру, а не по
 * числу итераций: стирание сектора занимает десятки миллисекунд, и «сколько
 * это в проходах цикла» зависит от частоты шины и оптимизации компилятора.
 */
static bool qspi_wait_busy_ms(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t  status = 0;

    do {
        if (!qspi_read_reg(W25Q_CMD_READ_STATUS1, &status)) {
            return false;
        }
        if ((status & 0x01u) == 0) {
            return true;
        }
    } while (HAL_GetTick() - start < timeout_ms);

    return false;
}

static bool qspi_wait_busy(void)
{
    return qspi_wait_busy_ms(QSPI_STATUS_TIMEOUT_MS);
}


/* Quad Enable нужен, иначе IO2/IO3 не работают как линии данных. */
static bool qspi_enable_quad(void)
{
    uint8_t status2 = 0;
    if (!qspi_read_reg(W25Q_CMD_READ_STATUS2, &status2)) {
        return false;
    }
    if (status2 & W25Q_STATUS2_QE) {
        return true;
    }

    if (!qspi_cmd_simple(W25Q_CMD_WRITE_ENABLE)) {
        return false;
    }

    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction     = W25Q_CMD_WRITE_STATUS2;
    cmd.AddressMode     = QSPI_ADDRESS_NONE;
    cmd.DataMode        = QSPI_DATA_1_LINE;
    cmd.NbData          = 1;
    cmd.SIOOMode        = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return false;
    }
    status2 |= W25Q_STATUS2_QE;
    if (HAL_QSPI_Transmit(&hqspi, &status2, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return false;
    }
    return qspi_wait_busy();
}

/* Переводит чип в memory-mapped режим: дальше читается как обычная память. */
static bool qspi_enter_memory_mapped(void)
{
    if (s_mm_active) {
        return true;
    }

    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction       = W25Q_CMD_FAST_READ_QUAD;
    cmd.AddressMode       = QSPI_ADDRESS_4_LINES;
    cmd.AddressSize       = QSPI_ADDRESS_24_BITS;
    cmd.AlternateByteMode = QSPI_ALTERNATE_BYTES_4_LINES;
    cmd.AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
    cmd.AlternateBytes    = 0x00;
    cmd.DataMode          = QSPI_DATA_4_LINES;
    cmd.DummyCycles       = 4;
    cmd.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;

    QSPI_MemoryMappedTypeDef mm = {0};
    mm.TimeOutActivation = QSPI_TIMEOUT_COUNTER_DISABLE;
    mm.TimeOutPeriod     = 0;

    if (HAL_QSPI_MemoryMapped(&hqspi, &cmd, &mm) != HAL_OK) {
        return false;
    }
    s_mm_active = true;
    return true;
}

/*
 * Выход из memory-mapped режима. Пока он включён, контроллер занят
 * бесконечной командой чтения и никакую другую не принимает — любое
 * стирание или запись начинается с Abort.
 */
static bool qspi_leave_memory_mapped(void)
{
    if (!s_mm_active) {
        return true;
    }
    if (HAL_QSPI_Abort(&hqspi) != HAL_OK) {
        return false;
    }
    s_mm_active = false;
    return true;
}


bool bsp_qspi_init(void)
{
    s_fail_stage = "";
    s_jedec_id[0] = s_jedec_id[1] = s_jedec_id[2] = 0;

    __HAL_RCC_QSPI_CLK_ENABLE();
    __HAL_RCC_QSPI_FORCE_RESET();
    __HAL_RCC_QSPI_RELEASE_RESET();

    qspi_gpio_init();

    hqspi.Instance = QUADSPI;
    hqspi.Init.ClockPrescaler     = 2;   /* 240 / (2+1) = 80 МГц */
    hqspi.Init.FifoThreshold      = 4;
    hqspi.Init.SampleShifting     = QSPI_SAMPLE_SHIFTING_HALFCYCLE;
    hqspi.Init.FlashSize          = QSPI_FLASH_SIZE_LOG2;
    hqspi.Init.ChipSelectHighTime = QSPI_CS_HIGH_TIME_2_CYCLE;
    hqspi.Init.ClockMode          = QSPI_CLOCK_MODE_0;
    hqspi.Init.FlashID            = QSPI_FLASH_ID_1;
    hqspi.Init.DualFlash          = QSPI_DUALFLASH_DISABLE;

    if (HAL_QSPI_Init(&hqspi) != HAL_OK) {
        return qspi_fail("HAL_QSPI_Init");
    }

    /* JEDEC ID: [вендор, тип, ёмкость]. Проверяем, что чип вообще отвечает. */
    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction     = W25Q_CMD_READ_ID;
    cmd.AddressMode     = QSPI_ADDRESS_NONE;
    cmd.DataMode        = QSPI_DATA_1_LINE;
    cmd.NbData          = sizeof(s_jedec_id);
    cmd.SIOOMode        = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return qspi_fail("команда 0x9F не ушла");
    }
    if (HAL_QSPI_Receive(&hqspi, s_jedec_id, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return qspi_fail("чип молчит на 0x9F");
    }
    /*
     * Вендора не сверяем со списком: конкретный чип зависит от ревизии платы
     * (встречались EF Winbond и C8 GigaDevice), а жёсткая проверка вендора
     * отключает исправную флешку из-за одного байта. Отсекаем только то,
     * что надо: 00 и FF — так «отвечают» висящие в воздухе линии, когда чипа
     * нет или он не выбран. А вот ёмкость сверяем строго: FlashSize и карта
     * памяти заданы под 16 МБ, и другой размер значит, что их пора править.
     */
    if (s_jedec_id[0] == 0x00 || s_jedec_id[0] == 0xFF) {
        return qspi_fail("чип не отвечает (линии в воздухе)");
    }
    if (s_jedec_id[2] != QSPI_FLASH_CAPACITY_ID) {
        return qspi_fail("ёмкость не 16 МБ");
    }

    if (!qspi_enable_quad()) {
        return qspi_fail("Quad Enable");
    }

    if (!qspi_enter_memory_mapped()) {
        return qspi_fail("memory-mapped режим");
    }

    s_ready = true;
    return true;
}

bool bsp_qspi_ready(void)
{
    return s_ready;
}

/* ------------------------------------------------------------------ */
/* Запись: стирание сектора + программирование страниц                 */
/* ------------------------------------------------------------------ */

static bool qspi_erase_sector(uint32_t addr)
{
    if (!qspi_cmd_simple(W25Q_CMD_WRITE_ENABLE)) {
        return false;
    }

    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction     = W25Q_CMD_SECTOR_ERASE_4K;
    cmd.AddressMode     = QSPI_ADDRESS_1_LINE;
    cmd.AddressSize     = QSPI_ADDRESS_24_BITS;
    cmd.Address         = addr;
    cmd.DataMode        = QSPI_DATA_NONE;
    cmd.SIOOMode        = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return false;
    }
    return qspi_wait_busy_ms(QSPI_ERASE_TIMEOUT_MS);
}

/* len не должен пересекать границу страницы: чип завернёт запись на её начало. */
static bool qspi_program_page(uint32_t addr, const uint8_t* data, uint32_t len)
{
    if (!qspi_cmd_simple(W25Q_CMD_WRITE_ENABLE)) {
        return false;
    }

    QSPI_CommandTypeDef cmd = {0};
    cmd.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    cmd.Instruction     = W25Q_CMD_PAGE_PROGRAM;
    cmd.AddressMode     = QSPI_ADDRESS_1_LINE;
    cmd.AddressSize     = QSPI_ADDRESS_24_BITS;
    cmd.Address         = addr;
    cmd.DataMode        = QSPI_DATA_1_LINE;
    cmd.NbData          = len;
    cmd.SIOOMode        = QSPI_SIOO_INST_EVERY_CMD;

    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return false;
    }
    if (HAL_QSPI_Transmit(&hqspi, (uint8_t*)data, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return false;
    }
    return qspi_wait_busy_ms(QSPI_PROGRAM_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* Блочный доступ (512 байт) для FatFs и USB MSC                       */
/* ------------------------------------------------------------------ */

/*
 * Кеш на один сектор стирания. FatFs и SCSI пишут по 512 байт, а стирать
 * NOR умеет только по 4 КБ, поэтому запись — это read-modify-write целого
 * сектора. Без кеша каждый 512-байтный блок означал бы отдельное стирание,
 * то есть в восемь раз больше стираний на тот же объём (и в восемь раз
 * быстрее выработанный ресурс чипа).
 */
static uint8_t  s_cache[QSPI_SECTOR_SIZE];
static uint32_t s_cache_addr = UINT32_MAX;   /* адрес сектора в чипе */
static bool     s_cache_dirty;

uint32_t bsp_qspi_block_count(void)
{
    return ORCA_QSPI_SIZE / QSPI_BLOCK_SIZE;
}

uint32_t bsp_qspi_erase_blocks(void)
{
    return QSPI_BLOCKS_PER_SECTOR;
}

/*
 * Кеш чипа D-Cache после записи обязателен: 0x90000000 помечен кешируемым,
 * и без сброса строк процессор ещё долго будет читать старое содержимое
 * сектора из кеша, а не то, что реально лежит во флешке.
 */
static void qspi_invalidate_cache(uint32_t addr, uint32_t len)
{
    SCB_InvalidateDCache_by_Addr((void*)(ORCA_QSPI_BASE + addr), (int32_t)len);
}

static bool qspi_cache_flush(void)
{
    if (!s_cache_dirty) {
        return true;
    }

    if (!qspi_leave_memory_mapped()) {
        return false;
    }

    bool ok = qspi_erase_sector(s_cache_addr);

    for (uint32_t off = 0; ok && off < QSPI_SECTOR_SIZE; off += QSPI_PAGE_SIZE) {
        /*
         * Страницы, которые и так остались пустыми после стирания, не
         * программируем: на почти пустом секторе это экономит большую
         * часть времени записи.
         */
        uint32_t i = 0;
        while (i < QSPI_PAGE_SIZE && s_cache[off + i] == 0xFFu) {
            i++;
        }
        if (i == QSPI_PAGE_SIZE) {
            continue;
        }
        ok = qspi_program_page(s_cache_addr + off, &s_cache[off], QSPI_PAGE_SIZE);
    }

    if (!qspi_enter_memory_mapped()) {
        ok = false;
    }
    qspi_invalidate_cache(s_cache_addr, QSPI_SECTOR_SIZE);

    s_cache_dirty = false;
    if (!ok) {
        /* Содержимое сектора после сбоя неизвестно — кеш больше не верен. */
        s_cache_addr = UINT32_MAX;
    }
    return ok;
}

static bool qspi_cache_load(uint32_t sector_addr)
{
    if (s_cache_addr == sector_addr) {
        return true;
    }
    if (!qspi_cache_flush()) {
        return false;
    }
    if (!qspi_enter_memory_mapped()) {
        return false;
    }

    memcpy(s_cache, (const void*)(ORCA_QSPI_BASE + sector_addr), QSPI_SECTOR_SIZE);
    s_cache_addr = sector_addr;
    return true;
}

bool bsp_qspi_block_read(uint8_t* buf, uint32_t lba, uint32_t count)
{
    if (!s_ready || (uint64_t)lba + count > bsp_qspi_block_count()) {
        return false;
    }
    if (!qspi_enter_memory_mapped()) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = (lba + i) * QSPI_BLOCK_SIZE;
        uint32_t sector_addr = addr & ~(QSPI_SECTOR_SIZE - 1u);

        /*
         * Незаписанный кеш новее чипа, поэтому читать надо из него:
         * иначе хост, записав блок и тут же перечитав его, получит старые
         * данные и решит, что файловая система побилась.
         */
        if (s_cache_dirty && s_cache_addr == sector_addr) {
            memcpy(buf + (size_t)i * QSPI_BLOCK_SIZE,
                   &s_cache[addr - sector_addr], QSPI_BLOCK_SIZE);
        } else {
            memcpy(buf + (size_t)i * QSPI_BLOCK_SIZE,
                   (const void*)(ORCA_QSPI_BASE + addr), QSPI_BLOCK_SIZE);
        }
    }
    return true;
}

bool bsp_qspi_block_write(const uint8_t* buf, uint32_t lba, uint32_t count)
{
    if (!s_ready || (uint64_t)lba + count > bsp_qspi_block_count()) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = (lba + i) * QSPI_BLOCK_SIZE;
        uint32_t sector_addr = addr & ~(QSPI_SECTOR_SIZE - 1u);

        if (!qspi_cache_load(sector_addr)) {
            return false;
        }
        memcpy(&s_cache[addr - sector_addr], buf + (size_t)i * QSPI_BLOCK_SIZE,
               QSPI_BLOCK_SIZE);
        s_cache_dirty = true;
    }
    return true;
}

bool bsp_qspi_sync(void)
{
    return qspi_cache_flush();
}

/* ------------------------------------------------------------------ */
/* Том FatFs "1:"                                                      */
/* ------------------------------------------------------------------ */

/*
 * Чип отдаётся то ядру (FatFs), то хосту (USB MSC), и одновременно с двух
 * сторон одну ФС трогать нельзя: хост кеширует FAT у себя и не ждёт, что
 * содержимое изменится под ним. Поэтому монтирование разъёмное — см.
 * OrcaKernel/link/orca_usb_msc.h.
 */
static FATFS s_fatfs;
static bool  s_mounted;

/* Общий служебный буфер: mkfs и проверка на «чистый чип» не пересекаются. */
static uint8_t s_work[QSPI_BLOCK_SIZE];

static bool qspi_looks_blank(void)
{
    if (!bsp_qspi_block_read(s_work, 0, 1)) {
        return false;
    }
    for (uint32_t i = 0; i < sizeof(s_work); i++) {
        if (s_work[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

bool bsp_qspi_mount(void)
{
    if (!s_ready) {
        return false;
    }
    if (s_mounted) {
        return true;
    }

    FRESULT res = f_mount(&s_fatfs, "1:", 1);

    /*
     * Пустой чип форматируем сами: иначе новая плата отдаёт хосту 16 МБ,
     * которые тот предлагает «отформатировать перед использованием».
     * Именно пустой — только все 0xFF в нулевом секторе. Если там лежит
     * что-то нераспознанное (чужая разметка, XIP-ресурсы без ФС), данные
     * не трогаем: пусть том не смонтируется, но останется как есть.
     */
    if (res == FR_NO_FILESYSTEM && qspi_looks_blank()) {
        MKFS_PARM opt = {
            .fmt     = FM_FAT,   /* 16 МБ — это FAT16, FAT32 здесь только мешает */
            .n_fat   = 1,
            .align   = QSPI_SECTOR_SIZE / QSPI_BLOCK_SIZE,
            .n_root  = 0,
            .au_size = QSPI_SECTOR_SIZE,  /* кластер = сектор стирания */
        };

        if (f_mkfs("1:", &opt, s_work, sizeof(s_work)) != FR_OK) {
            return false;
        }
        if (!bsp_qspi_sync()) {
            return false;
        }
        res = f_mount(&s_fatfs, "1:", 1);
        if (res == FR_OK) {
            f_setlabel("1:ORCA QSPI");
        }
    }

    if (res != FR_OK) {
        return false;
    }

    s_mounted = true;
    return true;
}

bool bsp_qspi_unmount(void)
{
    if (!s_mounted) {
        return true;
    }
    /* Сначала на чип, потом отдать: в кеше может лежать незаписанный сектор. */
    bool ok = bsp_qspi_sync();

    f_unmount("1:");
    s_mounted = false;
    return ok;
}

bool bsp_qspi_is_mounted(void)
{
    return s_mounted;
}



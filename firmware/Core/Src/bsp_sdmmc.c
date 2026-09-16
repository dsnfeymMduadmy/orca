#include "main.h"
#include "bsp_storage.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <string.h>

/*
 * microSD через SDMMC1, 4-битный режим.
 *   D0..D3  PC8 PC9 PC10 PC11  (AF12)
 *   CLK     PC12              (AF12)
 *   CMD     PD2               (AF12)
 * На линиях данных внешние подтяжки 10к (есть на плате).
 *
 * SDMMC_KER_CK = PLL1Q = 120 МГц, ClockDiv=2 -> 60 МГц (SDR25/High Speed).
 * Если карта капризная — поднять ClockDiv до 4 (30 МГц).
 *
 * DMA не используется: HAL_SD_ReadBlocks в polling-режиме на 60 МГц даёт
 * ~10 МБ/с, чего с запасом хватает и для загрузки .orca модулей, и для
 * USB FS (12 Мбит/с — это 1.5 МБ/с в теории). Буфер выравнивания лежит в
 * некешируемой RAM_D2.
 *
 * Наружу отдаётся блочный интерфейс (bsp_storage.h): карту читает не только
 * FatFs, но и USB MSC, который про файловую систему ничего не знает.
 */

#define SD_TIMEOUT_MS   5000u

static SD_HandleTypeDef hsd1;
static FATFS   s_fatfs;
static bool    s_ready;
static bool    s_mounted;

static uint8_t s_align_buf[BSP_BLOCK_SIZE] __attribute__((section(".dma_buffer"), aligned(32)));

/*
 * s_align_buf и состояние hsd1 — одни на всю систему, а вызывающих больше
 * одного: FatFs (со своим мьютексом), USB MSC и прямые вызовы bsp_sdmmc_*
 * из ядра. Мьютекса FatFs хватает только на FatFs, поэтому сериализуем
 * здесь: два одновременных обмена рвали общий буфер и отдавали половину
 * чужого блока.
 *
 * До старта планировщика (bsp_sdmmc_init -> bsp_sdmmc_mount -> disk_read)
 * блокировать нечем и не нужно: там мы гарантированно одни.
 */
static SemaphoreHandle_t s_lock;

static bool sd_lock(void)
{
    if (s_lock == NULL || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return false;
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void sd_unlock(bool locked)
{
    if (locked) {
        (void)xSemaphoreGive(s_lock);
    }
}

static void sdmmc_gpio_init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO1;

    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOD, &gpio);
}

bool bsp_sdmmc_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    __HAL_RCC_SDMMC1_CLK_ENABLE();
    sdmmc_gpio_init();

    hsd1.Instance                 = SDMMC1;
    hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd1.Init.BusWide             = SDMMC_BUS_WIDE_4B;
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
    hsd1.Init.ClockDiv            = 2;

    if (HAL_SD_Init(&hsd1) != HAL_OK) {
        return false;
    }
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK) {
        return false;
    }

    s_ready = true;

    return bsp_sdmmc_mount();
}

bool bsp_sdmmc_ready(void)
{
    return s_ready;
}

static bool sd_wait_ready(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (HAL_GetTick() - start < timeout_ms) {
        if (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER) {
            return true;
        }
    }
    return false;
}

uint32_t bsp_sdmmc_block_count(void)
{
    if (!s_ready) {
        return 0;
    }

    HAL_SD_CardInfoTypeDef info;
    HAL_SD_GetCardInfo(&hsd1, &info);
    return info.LogBlockNbr;
}

/*
 * Буфер вызывающего может быть невыровненным (например, структура на стеке
 * таска), а SDMMC требует выравнивания на 4 — такие обращения гоняем
 * поблочно через свой выровненный буфер.
 */
static bool sdmmc_block_read_locked(uint8_t* buf, uint32_t lba, uint32_t count)
{
    if (!s_ready || !sd_wait_ready(SD_TIMEOUT_MS)) {
        return false;
    }

    if (((uint32_t)(uintptr_t)buf & 3u) != 0) {
        for (uint32_t i = 0; i < count; i++) {
            if (HAL_SD_ReadBlocks(&hsd1, s_align_buf, lba + i, 1, SD_TIMEOUT_MS) != HAL_OK) {
                return false;
            }
            if (!sd_wait_ready(SD_TIMEOUT_MS)) {
                return false;
            }
            memcpy(buf + (size_t)i * BSP_BLOCK_SIZE, s_align_buf, BSP_BLOCK_SIZE);
        }
        return true;
    }

    if (HAL_SD_ReadBlocks(&hsd1, buf, lba, count, SD_TIMEOUT_MS) != HAL_OK) {
        return false;
    }
    return sd_wait_ready(SD_TIMEOUT_MS);
}

bool bsp_sdmmc_block_read(uint8_t* buf, uint32_t lba, uint32_t count)
{
    bool locked = sd_lock();
    bool ok = sdmmc_block_read_locked(buf, lba, count);
    sd_unlock(locked);
    return ok;
}

static bool sdmmc_block_write_locked(const uint8_t* buf, uint32_t lba, uint32_t count)
{
    if (!s_ready || !sd_wait_ready(SD_TIMEOUT_MS)) {
        return false;
    }

    if (((uint32_t)(uintptr_t)buf & 3u) != 0) {
        for (uint32_t i = 0; i < count; i++) {
            memcpy(s_align_buf, buf + (size_t)i * BSP_BLOCK_SIZE, BSP_BLOCK_SIZE);
            if (HAL_SD_WriteBlocks(&hsd1, s_align_buf, lba + i, 1, SD_TIMEOUT_MS) != HAL_OK) {
                return false;
            }
            if (!sd_wait_ready(SD_TIMEOUT_MS)) {
                return false;
            }
        }
        return true;
    }

    if (HAL_SD_WriteBlocks(&hsd1, (uint8_t*)buf, lba, count, SD_TIMEOUT_MS) != HAL_OK) {
        return false;
    }
    return sd_wait_ready(SD_TIMEOUT_MS);
}

bool bsp_sdmmc_block_write(const uint8_t* buf, uint32_t lba, uint32_t count)
{
    bool locked = sd_lock();
    bool ok = sdmmc_block_write_locked(buf, lba, count);
    sd_unlock(locked);
    return ok;
}

/*
 * Своего кеша записи здесь нет: карта отчитывается о конце записи сама, так
 * что «сброс» — это просто дождаться, пока она освободится.
 */
bool bsp_sdmmc_sync(void)
{
    bool locked = sd_lock();
    bool ok = s_ready && sd_wait_ready(SD_TIMEOUT_MS);
    sd_unlock(locked);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Том FatFs "0:"                                                      */
/* ------------------------------------------------------------------ */

bool bsp_sdmmc_mount(void)
{
    if (!s_ready) {
        return false;
    }
    if (s_mounted) {
        return true;
    }
    if (f_mount(&s_fatfs, "0:", 1) != FR_OK) {
        return false;
    }

    s_mounted = true;
    return true;
}

bool bsp_sdmmc_unmount(void)
{
    if (!s_mounted) {
        return true;
    }
    bool ok = bsp_sdmmc_sync();

    f_unmount("0:");
    s_mounted = false;
    return ok;
}

bool bsp_sdmmc_is_mounted(void)
{
    return s_mounted;
}

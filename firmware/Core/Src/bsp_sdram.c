#include "main.h"
#include <stdbool.h>

/*
 * W9825G6KH-6I: 32 MB (4 банка x 4M x 16 бит), 16-битная шина, CL3.
 * Подключён к FMC SDRAM Bank1 -> адрес 0xC0000000.
 *
 * Пины (LQFP176):
 *   A0..A5    PF0 PF1 PF2 PF3 PF4 PF5
 *   A6..A9    PF12 PF13 PF14 PF15
 *   A10,A11   PG0 PG1
 *   A12       PG2                  <-- в описании платы не был указан, но
 *                                      W9825G6KH требует 13 строк адреса
 *   BA0,BA1   PG4 PG5              <-- тоже обязательны (4 банка)
 *   D0..D3    PD14 PD15 PD0 PD1
 *   D4..D12   PE7..PE15
 *   D13..D15  PD8 PD9 PD10
 *   NBL0,NBL1 PE0 PE1
 *   SDNWE     PC0
 *   SDNRAS    PF11
 *   SDNCAS    PG15
 *   SDCLK     PG8
 *   SDCKE0    PH2
 *   SDNE0     PH3
 *
 * SDCLK = FMC_KER_CK / 2 = 240 / 2 = 120 МГц (8.33 нс). Если на плате
 * длинные дорожки и тест памяти сыпется — переключить на SDCLK_PERIOD_3
 * (80 МГц) и пересчитать REFRESH_COUNT.
 */

#define SDRAM_BANK_ADDR   0xC0000000UL
#define SDRAM_SIZE_BYTES  (32u * 1024u * 1024u)

/*
 * Refresh: 8192 строк / 64 мс = 7.8125 мкс на строку.
 * COUNT = 7.8125e-6 * 120e6 - 20 = 937 - 20 = 917.
 */
#define SDRAM_REFRESH_COUNT  917u

/* Поля mode register W9825G6KH (JEDEC-совместимые). */
#define SDRAM_MODEREG_BURST_LENGTH_1            0x0000u
#define SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL     0x0000u
#define SDRAM_MODEREG_CAS_LATENCY_3             0x0030u
#define SDRAM_MODEREG_OPERATING_MODE_STANDARD   0x0000u
#define SDRAM_MODEREG_WRITEBURST_MODE_SINGLE    0x0200u

static SDRAM_HandleTypeDef hsdram1;

static void sdram_gpio_init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_FMC;

    gpio.Pin = GPIO_PIN_0;                                    /* SDNWE */
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = GPIO_PIN_0  | GPIO_PIN_1  | GPIO_PIN_8  |       /* D2 D3 D13 */
               GPIO_PIN_9  | GPIO_PIN_10 | GPIO_PIN_14 |       /* D14 D15 D0 */
               GPIO_PIN_15;                                    /* D1 */
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Pin = GPIO_PIN_0  | GPIO_PIN_1  |                     /* NBL0 NBL1 */
               GPIO_PIN_7  | GPIO_PIN_8  | GPIO_PIN_9  |       /* D4 D5 D6 */
               GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |       /* D7 D8 D9 */
               GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;        /* D10 D11 D12 */
    HAL_GPIO_Init(GPIOE, &gpio);

    gpio.Pin = GPIO_PIN_0  | GPIO_PIN_1  | GPIO_PIN_2  |       /* A0 A1 A2 */
               GPIO_PIN_3  | GPIO_PIN_4  | GPIO_PIN_5  |       /* A3 A4 A5 */
               GPIO_PIN_11 |                                   /* SDNRAS */
               GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 |       /* A6 A7 A8 */
               GPIO_PIN_15;                                    /* A9 */
    HAL_GPIO_Init(GPIOF, &gpio);

    gpio.Pin = GPIO_PIN_0  | GPIO_PIN_1  | GPIO_PIN_2  |       /* A10 A11 A12 */
               GPIO_PIN_4  | GPIO_PIN_5  |                     /* BA0 BA1 */
               GPIO_PIN_8  |                                   /* SDCLK */
               GPIO_PIN_15;                                    /* SDNCAS */
    HAL_GPIO_Init(GPIOG, &gpio);

    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;                        /* SDCKE0 SDNE0 */
    HAL_GPIO_Init(GPIOH, &gpio);
}

static bool sdram_send_command(uint32_t mode, uint32_t target,
                               uint32_t refresh, uint32_t modereg)
{
    FMC_SDRAM_CommandTypeDef cmd;
    cmd.CommandMode            = mode;
    cmd.CommandTarget          = target;
    cmd.AutoRefreshNumber      = refresh;
    cmd.ModeRegisterDefinition = modereg;
    return HAL_SDRAM_SendCommand(&hsdram1, &cmd, 1000) == HAL_OK;
}

static bool sdram_sequence(void)
{
    /* 1. Clock enable, пауза >= 100 мкс. */
    if (!sdram_send_command(FMC_SDRAM_CMD_CLK_ENABLE, FMC_SDRAM_CMD_TARGET_BANK1, 1, 0)) {
        return false;
    }
    HAL_Delay(1);

    /* 2. Precharge all. */
    if (!sdram_send_command(FMC_SDRAM_CMD_PALL, FMC_SDRAM_CMD_TARGET_BANK1, 1, 0)) {
        return false;
    }

    /* 3. Два auto-refresh цикла. */
    if (!sdram_send_command(FMC_SDRAM_CMD_AUTOREFRESH_MODE, FMC_SDRAM_CMD_TARGET_BANK1, 8, 0)) {
        return false;
    }

    /*
     * 4. Load mode register:
     *    burst length 1, sequential, CAS latency 3, standard op, single write.
     */
    uint32_t modereg = SDRAM_MODEREG_BURST_LENGTH_1        |
                       SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL |
                       SDRAM_MODEREG_CAS_LATENCY_3         |
                       SDRAM_MODEREG_OPERATING_MODE_STANDARD |
                       SDRAM_MODEREG_WRITEBURST_MODE_SINGLE;
    if (!sdram_send_command(FMC_SDRAM_CMD_LOAD_MODE, FMC_SDRAM_CMD_TARGET_BANK1, 1, modereg)) {
        return false;
    }

    HAL_SDRAM_ProgramRefreshRate(&hsdram1, SDRAM_REFRESH_COUNT);
    return true;
}

/* Быстрый memtest: адресные линии (walking 1) + случайный паттерн по всей памяти. */
static bool sdram_selftest(void)
{
    volatile uint32_t* mem = (volatile uint32_t*)SDRAM_BANK_ADDR;

    /* Проверка адресных линий — ловит непропаянные/перепутанные A*/
    for (uint32_t bit = 0; (4u << bit) <= SDRAM_SIZE_BYTES; bit++) {
        uint32_t idx = (1u << bit);
        if (idx * 4u >= SDRAM_SIZE_BYTES) {
            break;
        }
        mem[idx] = 0xA5A50000u | bit;
    }
    mem[0] = 0xDEADBEEFu;
    for (uint32_t bit = 0; (4u << bit) <= SDRAM_SIZE_BYTES; bit++) {
        uint32_t idx = (1u << bit);
        if (idx * 4u >= SDRAM_SIZE_BYTES) {
            break;
        }
        if (mem[idx] != (0xA5A50000u | bit)) {
            return false;
        }
    }
    if (mem[0] != 0xDEADBEEFu) {
        return false;
    }

    /* Разреженная проверка данных по всему объёму (каждые 4 КБ). */
    const uint32_t step = 1024u;  /* в словах = 4 КБ */
    uint32_t words = SDRAM_SIZE_BYTES / 4u;
    uint32_t lfsr = 0x12345678u;
    for (uint32_t i = 0; i < words; i += step) {
        mem[i] = lfsr;
        lfsr = (lfsr << 1) ^ ((lfsr & 0x80000000u) ? 0x04C11DB7u : 0u);
    }
    lfsr = 0x12345678u;
    for (uint32_t i = 0; i < words; i += step) {
        if (mem[i] != lfsr) {
            return false;
        }
        lfsr = (lfsr << 1) ^ ((lfsr & 0x80000000u) ? 0x04C11DB7u : 0u);
    }

    return true;
}

bool bsp_sdram_init(void)
{
    __HAL_RCC_FMC_CLK_ENABLE();
    sdram_gpio_init();

    hsdram1.Instance = FMC_SDRAM_DEVICE;

    hsdram1.Init.SDBank             = FMC_SDRAM_BANK1;
    hsdram1.Init.ColumnBitsNumber   = FMC_SDRAM_COLUMN_BITS_NUM_9;
    hsdram1.Init.RowBitsNumber      = FMC_SDRAM_ROW_BITS_NUM_13;
    hsdram1.Init.MemoryDataWidth    = FMC_SDRAM_MEM_BUS_WIDTH_16;
    hsdram1.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;
    hsdram1.Init.CASLatency         = FMC_SDRAM_CAS_LATENCY_3;
    hsdram1.Init.WriteProtection    = FMC_SDRAM_WRITE_PROTECTION_DISABLE;
    hsdram1.Init.SDClockPeriod      = FMC_SDRAM_CLOCK_PERIOD_2;
    hsdram1.Init.ReadBurst          = FMC_SDRAM_RBURST_ENABLE;
    hsdram1.Init.ReadPipeDelay      = FMC_SDRAM_RPIPE_DELAY_0;

    /* Тайминги для -6I на 120 МГц (округление вверх, значения в тактах). */
    FMC_SDRAM_TimingTypeDef timing;
    timing.LoadToActiveDelay    = 2;   /* tMRD  = 2 такта      */
    timing.ExitSelfRefreshDelay = 9;   /* tXSR >= 72 нс        */
    timing.SelfRefreshTime      = 6;   /* tRAS >= 42 нс        */
    timing.RowCycleDelay        = 8;   /* tRC  >= 60 нс        */
    timing.WriteRecoveryTime    = 2;   /* tWR                  */
    timing.RPDelay              = 3;   /* tRP  >= 18 нс        */
    timing.RCDDelay             = 3;   /* tRCD >= 18 нс        */

    if (HAL_SDRAM_Init(&hsdram1, &timing) != HAL_OK) {
        return false;
    }

    if (!sdram_sequence()) {
        return false;
    }

    return sdram_selftest();
}

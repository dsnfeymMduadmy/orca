#include "board.h"
#include "bsp_storage.h"
#include "main.h"
#include <string.h>

static void SystemClock_Config(void);
static void MPU_Config(void);
static void GPIO_Init(void);

void board_init(void)
{
    /*
     * По умолчанию MemManage/BusFault/UsageFault выключены и любой из них
     * приходит как HardFault — то есть "что-то упало", без указания что.
     * Включаем их отдельно: тогда обработчик сразу говорит, это нарушение
     * MPU, обращение к несуществующей памяти или неверная инструкция.
     */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk |
                  SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;

    MPU_Config();
    SCB_EnableICache();
    SCB_EnableDCache();
    
    SystemClock_Config();
    GPIO_Init();
}

static void GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    
    GPIO_InitStruct.Pin = LED0_Pin | LED1_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    HAL_GPIO_WritePin(GPIOB, LED0_Pin | LED1_Pin, GPIO_PIN_SET);
    
    GPIO_InitStruct.Pin = BTN_WK_UP_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(BTN_WK_UP_GPIO_Port, &GPIO_InitStruct);
    
    GPIO_InitStruct.Pin = BTN_KEY0_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(BTN_KEY0_GPIO_Port, &GPIO_InitStruct);
}

void board_led_set(uint8_t led, bool on)
{
    uint16_t pin = (led == 0) ? LED0_Pin : LED1_Pin;
    HAL_GPIO_WritePin(GPIOB, pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

bool board_button_pressed(uint8_t btn)
{
    if (btn == 0) {
        return HAL_GPIO_ReadPin(BTN_WK_UP_GPIO_Port, BTN_WK_UP_Pin) == GPIO_PIN_SET;
    } else {
        return HAL_GPIO_ReadPin(BTN_KEY0_GPIO_Port, BTN_KEY0_Pin) == GPIO_PIN_RESET;
    }
}

extern bool bsp_sdram_init(void);

static bool s_sdram_ready;

bool board_sdram_init(void)
{
    s_sdram_ready = bsp_sdram_init();
    return s_sdram_ready;
}

bool board_sdram_ready(void)
{
    return s_sdram_ready;
}

bool board_qspi_init(void)
{
    return bsp_qspi_init();
}

const char* board_qspi_error(void)
{
    return bsp_qspi_error();
}

const uint8_t* board_qspi_jedec_id(void)
{
    return bsp_qspi_jedec_id();
}

bool board_sdmmc_init(void)
{
    return bsp_sdmmc_init();
}

bool board_sdmmc_ready(void)
{
    return bsp_sdmmc_is_mounted();
}

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef       RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef       RCC_ClkInitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInit     = {0};

    /* Питание ядра: внешние LDO (RT9193) дают 3.3V, внутренний регулятор — LDO-режим. */
    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
    }

    /*
     * HSE = 25 МГц (кварц на PH0/PH1).
     * PLL1: 25 / M5 = 5 МГц -> xN192 = 960 МГц VCO -> /P2 = 480 МГц SYSCLK.
     *       /Q8 = 120 МГц (SDMMC1), /R2 = 480 МГц.
     * LSE = 32.768 кГц (PC14/PC15) для RTC.
     * HSI48 — трансиверу USB (см. UsbClockSelection ниже). Отдельный
     * генератор, потому что PLL3 отдана пиксельклоку LTDC, а точность
     * HSI48 добирает CRS по SOF хоста — этим занимается bsp_usb.c.
     */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_LSE |
                                       RCC_OSCILLATORTYPE_HSI48;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.LSEState       = RCC_LSE_ON;
    RCC_OscInitStruct.HSI48State     = RCC_HSI48_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM       = 5;
    RCC_OscInitStruct.PLL.PLLN       = 192;
    RCC_OscInitStruct.PLL.PLLP       = 2;
    RCC_OscInitStruct.PLL.PLLQ       = 8;
    RCC_OscInitStruct.PLL.PLLR       = 2;
    RCC_OscInitStruct.PLL.PLLRGE     = RCC_PLL1VCIRANGE_2;   /* вход 4..8 МГц */
    RCC_OscInitStruct.PLL.PLLVCOSEL  = RCC_PLL1VCOWIDE;      /* VCO 192..960 МГц */
    RCC_OscInitStruct.PLL.PLLFRACN   = 0;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    /* SYSCLK 480, HCLK/AXI 240, APBx 120 МГц. */
    RCC_ClkInitStruct.ClockType     = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                      RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                                      RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
    RCC_ClkInitStruct.SYSCLKSource  = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
    RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK) {
        Error_Handler();
    }

    /*
     * PLL3 — под LTDC (пиксельклок задаётся уже в драйвере дисплея под
     * конкретную панель; здесь только источники для FMC/QSPI/SDMMC/USART/USB).
     */
    PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART1 | RCC_PERIPHCLK_SDMMC |
                                         RCC_PERIPHCLK_QSPI   | RCC_PERIPHCLK_FMC   |
                                         RCC_PERIPHCLK_I2C2   | RCC_PERIPHCLK_USB   |
                                         RCC_PERIPHCLK_RTC;
    PeriphClkInit.Usart16ClockSelection = RCC_USART16CLKSOURCE_D2PCLK2;
    PeriphClkInit.SdmmcClockSelection   = RCC_SDMMCCLKSOURCE_PLL;      /* PLL1Q = 120 МГц */
    PeriphClkInit.QspiClockSelection    = RCC_QSPICLKSOURCE_D1HCLK;    /* 240 МГц */
    PeriphClkInit.FmcClockSelection     = RCC_FMCCLKSOURCE_D1HCLK;     /* 240 МГц -> SDCLK 120 */
    PeriphClkInit.I2c123ClockSelection  = RCC_I2C123CLKSOURCE_D2PCLK1;
    PeriphClkInit.UsbClockSelection     = RCC_USBCLKSOURCE_HSI48;
    PeriphClkInit.RTCClockSelection     = RCC_RTCCLKSOURCE_LSE;

    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) {
        Error_Handler();
    }

    __HAL_RCC_CSI_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    HAL_EnableCompensationCell();
}

/*
 * MPU. Ключевой момент: арена загружаемых модулей лежит в SDRAM и из неё
 * ИСПОЛНЯЕТСЯ код (orca_loader). Поэтому SDRAM целиком executable, а
 * не-исполняемыми точечно помечаются framebuffer и куча GUI.
 * Регион с большим номером перекрывает меньший.
 */
static void MPU_Config(void)
{
    MPU_Region_InitTypeDef MPU_InitStruct = {0};

    HAL_MPU_Disable();

    /* 0: вся SDRAM — Normal, write-back/write-allocate, ИСПОЛНЯЕМАЯ. */
    MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
    MPU_InitStruct.Number           = MPU_REGION_NUMBER0;
    MPU_InitStruct.BaseAddress      = 0xC0000000;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_32MB;
    MPU_InitStruct.SubRegionDisable = 0x00;
    MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
    MPU_InitStruct.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
    MPU_InitStruct.IsBufferable     = MPU_ACCESS_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    /* 1: framebuffer 4 MB — write-through, чтобы LTDC видел свежие пиксели. */
    MPU_InitStruct.Number           = MPU_REGION_NUMBER1;
    MPU_InitStruct.BaseAddress      = 0xC0000000;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_4MB;
    MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL0;
    MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
    MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    /* 2: куча GUI 2 MB — обычная кешируемая, но код оттуда не исполняется. */
    MPU_InitStruct.Number           = MPU_REGION_NUMBER2;
    MPU_InitStruct.BaseAddress      = 0xC0400000;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_2MB;
    MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
    MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
    MPU_InitStruct.IsBufferable     = MPU_ACCESS_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    /* 3: QSPI memory-mapped 16 MB — read-only, исполняемая (XIP-ресурсы). */
    MPU_InitStruct.Number           = MPU_REGION_NUMBER3;
    MPU_InitStruct.BaseAddress      = 0x90000000;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_16MB;
    MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL0;
    MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO_URO;
    MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
    MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
    MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    /*
     * 4 и 5: RAM_D2 — некешируемая, под DMA-буферы SDMMC/периферии.
     * RAM_D2 в линкере 288 КБ, а размер региона MPU обязан быть степенью
     * двойки: одним регионом закрываются только первые 256 КБ. Остаток
     * 32 КБ закрываем вторым регионом — иначе DMA-буфер, который однажды
     * туда попадёт, окажется в кешируемой зоне, и это будут «случайные»
     * повреждения данных на SD, которые ловятся неделями.
     */
    MPU_InitStruct.Number           = MPU_REGION_NUMBER4;
    MPU_InitStruct.BaseAddress      = 0x30000000;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_256KB;
    MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    /* 5: хвост RAM_D2 0x30040000..0x30048000 — те же атрибуты. */
    MPU_InitStruct.Number           = MPU_REGION_NUMBER5;
    MPU_InitStruct.BaseAddress      = 0x30040000;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_32KB;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

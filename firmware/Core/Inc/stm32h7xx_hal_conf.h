#ifndef STM32H7XX_HAL_CONF_H
#define STM32H7XX_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Orca OS — конфигурация HAL для STM32H743II.
 *
 * Включены только модули, которые реально собираются (см. HAL_SOURCES в
 * CMakeLists.txt). Добавляя модуль сюда, добавь и его .c в HAL_SOURCES,
 * иначе получишь ошибку линковки вместо ошибки компиляции.
 * ========================================================================== */

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_MDMA_MODULE_ENABLED
#define HAL_PCD_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_QSPI_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_RTC_MODULE_ENABLED
#define HAL_SD_MODULE_ENABLED
#define HAL_SDRAM_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* --------------------------------------------------------------------------
 * Тактирование. Значения должны совпадать с SystemClock_Config() в board.c.
 * -------------------------------------------------------------------------- */
#define HSE_VALUE            (25000000UL)   /* Кварц PH0/PH1 */
#define LSE_VALUE            (32768UL)      /* Кварц PC14/PC15 (RTC) */
#define HSE_STARTUP_TIMEOUT  (5000UL)
#define LSE_STARTUP_TIMEOUT  (5000UL)
#define EXTERNAL_CLOCK_VALUE (12288000UL)
#define HSI_VALUE            (64000000UL)
#define CSI_VALUE            (4000000UL)
#define LSI_VALUE            (32000UL)
#define HSI48_VALUE          (48000000UL)

#define VDD_VALUE                 (3300UL)
#define TICK_INT_PRIORITY         (0x0FUL)
#define USE_RTOS                  0U
#define PREFETCH_ENABLE           1U
#define INSTRUCTION_CACHE_ENABLE  1U
#define DATA_CACHE_ENABLE         1U

#define USE_HAL_DMA_REGISTER_CALLBACKS     0U
#define USE_HAL_PCD_REGISTER_CALLBACKS     0U
#define USE_HAL_QSPI_REGISTER_CALLBACKS    0U
#define USE_HAL_RCC_REGISTER_CALLBACKS     0U
#define USE_HAL_SD_REGISTER_CALLBACKS      0U
#define USE_HAL_SDRAM_REGISTER_CALLBACKS   0U
#define USE_HAL_UART_REGISTER_CALLBACKS    0U

/* Проверки параметров HAL отключены: экономия ~10 КБ FLASH, а ошибки
 * конфигурации периферии всё равно ловятся возвратом != HAL_OK. */
#define assert_param(expr) ((void)0U)

/* --------------------------------------------------------------------------
 * Заголовки включённых модулей
 * -------------------------------------------------------------------------- */

#ifdef HAL_CORTEX_MODULE_ENABLED
#include "stm32h7xx_hal_cortex.h"
#endif

#ifdef HAL_DMA_MODULE_ENABLED
#include "stm32h7xx_hal_dma.h"
#endif

#ifdef HAL_FLASH_MODULE_ENABLED
#include "stm32h7xx_hal_flash.h"
#endif

#ifdef HAL_GPIO_MODULE_ENABLED
#include "stm32h7xx_hal_gpio.h"
#endif

#ifdef HAL_MDMA_MODULE_ENABLED
#include "stm32h7xx_hal_mdma.h"
#endif

#ifdef HAL_PCD_MODULE_ENABLED
#include "stm32h7xx_hal_pcd.h"
#endif

#ifdef HAL_PWR_MODULE_ENABLED
#include "stm32h7xx_hal_pwr.h"
#endif

#ifdef HAL_QSPI_MODULE_ENABLED
#include "stm32h7xx_hal_qspi.h"
#endif

#ifdef HAL_RCC_MODULE_ENABLED
#include "stm32h7xx_hal_rcc.h"
#endif

#ifdef HAL_RTC_MODULE_ENABLED
#include "stm32h7xx_hal_rtc.h"
#endif

#ifdef HAL_SD_MODULE_ENABLED
#include "stm32h7xx_hal_sd.h"
#endif

#ifdef HAL_SDRAM_MODULE_ENABLED
#include "stm32h7xx_hal_sdram.h"
#endif

#ifdef HAL_UART_MODULE_ENABLED
#include "stm32h7xx_hal_uart.h"
#endif

#ifdef __cplusplus
}
#endif

#endif /* STM32H7XX_HAL_CONF_H */

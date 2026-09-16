#ifndef USBD_CONF_H
#define USBD_CONF_H

#include "stm32h7xx_hal.h"
#include <stdint.h>
#include <string.h>

/*
 * Настройка USB Device middleware (stm32_mw_usb_device). Файл читается
 * самим апстримом — имена макросов задаёт он, не мы; шаблон лежит в
 * _deps/stm32_usb_device-src/Core/Inc/usbd_conf_template.h.
 *
 * Устройство одно: Mass Storage с тремя LUN'ами (внутренняя FLASH, QSPI,
 * SD-карта) — см. OrcaKernel/link/orca_usb_msc.h.
 */

#define USBD_MAX_NUM_INTERFACES     1U
#define USBD_MAX_NUM_CONFIGURATION  1U
#define USBD_MAX_STR_DESC_SIZ       64U
#define USBD_SELF_POWERED           1U   /* плата питается не от шины */
#define USBD_DEBUG_LEVEL            0U   /* printf из ISR нам не нужен */

/*
 * Размер буфера одной передачи BOT. Лежит внутри USBD_MSC_BOT_HandleTypeDef,
 * то есть это статическая RAM, зато за одно прерывание уходит 8 секторов
 * вместо одного: на FS-скорости (1.2 МБ/с) разница между 512 и 4096 —
 * примерно втрое по реальной пропускной способности.
 */
#define MSC_MEDIA_PACKET            4096U

/*
 * Максимальный номер LUN, а не их количество: массивы в классе объявлены
 * как [MSC_BOT_MAX_LUN + 1]. Три тома -> 2.
 */
#define MSC_BOT_MAX_LUN             2U

/* Память под класс — статическая: malloc в прошивке нет. */
#define USBD_malloc                 (void *)USBD_static_malloc
#define USBD_free                   USBD_static_free
#define USBD_memset                 memset
#define USBD_memcpy                 memcpy
#define USBD_Delay                  HAL_Delay

#define USBD_UsrLog(...)            do {} while (0)
#define USBD_ErrLog(...)            do {} while (0)
#define USBD_DbgLog(...)            do {} while (0)

void* USBD_static_malloc(uint32_t size);
void  USBD_static_free(void* p);

#endif

#include "usbd_core.h"
#include "usbd_ctlreq.h"

/*
 * Дескрипторы USB-устройства. Наружу устройство называется Orca — это имя
 * видит хост в списке устройств; названия самих томов задаются метками FAT
 * и строками INQUIRY в orca_usb_msc.c.
 *
 * VID/PID — штатная пара ST для их же MSC-примеров. Своего VID у проекта
 * нет, а выдумывать чужой нельзя: с идентификатором ST устройство честно
 * опознаётся как STM32 с классом Mass Storage и подхватывается стандартным
 * драйвером хоста без inf-файлов и правил udev.
 */
#define USBD_VID            0x0483u
#define USBD_PID            0x5720u
#define USBD_LANGID_STRING  0x0409u   /* en-US: другой локали у нас нет */

#define USBD_MANUFACTURER   "Orca"
#define USBD_PRODUCT        "Orca"
#define USBD_CONFIGURATION  "Orca MSC"
#define USBD_INTERFACE      "Orca Storage"

static uint8_t* desc_device(USBD_SpeedTypeDef speed, uint16_t* length);
static uint8_t* desc_langid(USBD_SpeedTypeDef speed, uint16_t* length);
static uint8_t* desc_manufacturer(USBD_SpeedTypeDef speed, uint16_t* length);
static uint8_t* desc_product(USBD_SpeedTypeDef speed, uint16_t* length);
static uint8_t* desc_serial(USBD_SpeedTypeDef speed, uint16_t* length);
static uint8_t* desc_configuration(USBD_SpeedTypeDef speed, uint16_t* length);
static uint8_t* desc_interface(USBD_SpeedTypeDef speed, uint16_t* length);

USBD_DescriptorsTypeDef orca_usb_descriptors = {
    desc_device,
    desc_langid,
    desc_manufacturer,
    desc_product,
    desc_serial,
    desc_configuration,
    desc_interface,
};

/* Выравнивание на 4: дескрипторы уходят в FIFO 32-битными словами. */
static __attribute__((aligned(4))) uint8_t s_device_desc[USB_LEN_DEV_DESC] = {
    USB_LEN_DEV_DESC,           /* bLength */
    USB_DESC_TYPE_DEVICE,       /* bDescriptorType */
    0x00, 0x02,                 /* bcdUSB 2.00 */
    0x00,                       /* bDeviceClass: класс задаёт интерфейс */
    0x00,                       /* bDeviceSubClass */
    0x00,                       /* bDeviceProtocol */
    USB_MAX_EP0_SIZE,           /* bMaxPacketSize0 */
    LOBYTE(USBD_VID), HIBYTE(USBD_VID),
    LOBYTE(USBD_PID), HIBYTE(USBD_PID),
    0x00, 0x02,                 /* bcdDevice 2.00 */
    USBD_IDX_MFC_STR,           /* iManufacturer */
    USBD_IDX_PRODUCT_STR,       /* iProduct */
    USBD_IDX_SERIAL_STR,        /* iSerialNumber */
    USBD_MAX_NUM_CONFIGURATION, /* bNumConfigurations */
};

static __attribute__((aligned(4))) uint8_t s_langid_desc[USB_LEN_LANGID_STR_DESC] = {
    USB_LEN_LANGID_STR_DESC,
    USB_DESC_TYPE_STRING,
    LOBYTE(USBD_LANGID_STRING), HIBYTE(USBD_LANGID_STRING),
};

/* Общий буфер под UTF-16 строку: запросы к строкам идут по одному. */
static __attribute__((aligned(4))) uint8_t s_str_desc[USBD_MAX_STR_DESC_SIZ];

static uint8_t* desc_device(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    *length = sizeof(s_device_desc);
    return s_device_desc;
}

static uint8_t* desc_langid(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    *length = sizeof(s_langid_desc);
    return s_langid_desc;
}

static uint8_t* desc_manufacturer(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    USBD_GetString((uint8_t*)USBD_MANUFACTURER, s_str_desc, length);
    return s_str_desc;
}

static uint8_t* desc_product(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    USBD_GetString((uint8_t*)USBD_PRODUCT, s_str_desc, length);
    return s_str_desc;
}

/*
 * Серийный номер — из заводского уникального ID (96 бит, 0x1FF1E800).
 * Он должен быть разным у разных плат: по серийнику хост различает
 * устройства и помнит буквы дисков, а одинаковый номер у двух Orca
 * ломает монтирование обеих.
 */
static uint8_t* desc_serial(USBD_SpeedTypeDef speed, uint16_t* length)
{
    static char serial[25];
    static const char hex[] = "0123456789ABCDEF";

    (void)speed;

    if (serial[0] == 0) {
        const uint32_t uid[3] = {
            HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2(),
        };
        char* p = serial;

        for (uint32_t w = 0; w < 3; w++) {
            for (int32_t nib = 7; nib >= 0; nib--) {
                *p++ = hex[(uid[w] >> (nib * 4)) & 0x0Fu];
            }
        }
        *p = 0;
    }

    USBD_GetString((uint8_t*)serial, s_str_desc, length);
    return s_str_desc;
}

static uint8_t* desc_configuration(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    USBD_GetString((uint8_t*)USBD_CONFIGURATION, s_str_desc, length);
    return s_str_desc;
}

static uint8_t* desc_interface(USBD_SpeedTypeDef speed, uint16_t* length)
{
    (void)speed;
    USBD_GetString((uint8_t*)USBD_INTERFACE, s_str_desc, length);
    return s_str_desc;
}

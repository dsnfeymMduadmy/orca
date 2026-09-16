#ifndef ORCA_MEMMAP_H
#define ORCA_MEMMAP_H

#include <stdint.h>

/* Внутренняя FLASH: ядро Orca (2 MB @ 0x08000000) */
#define ORCA_FLASH_BASE          0x08000000UL
#define ORCA_FLASH_SIZE          (2u * 1024u * 1024u)

/* DTCM (128 KB @ 0x20000000) — стеки критичных задач ядра */
#define ORCA_DTCM_BASE           0x20000000UL
#define ORCA_DTCM_SIZE           (128u * 1024u)

/* AXI SRAM (512 KB @ 0x24000000) — куча ядра, очереди, буферы */
#define ORCA_AXI_SRAM_BASE       0x24000000UL
#define ORCA_AXI_SRAM_SIZE       (512u * 1024u)

/* Внешняя QSPI FLASH (16 MB, memory-mapped @ 0x90000000) — ресурсы ОС */
#define ORCA_QSPI_BASE           0x90000000UL
#define ORCA_QSPI_SIZE           (16u * 1024u * 1024u)

/* Внешняя SDRAM (32 MB через FMC @ 0xC0000000) */
#define ORCA_SDRAM_BASE          0xC0000000UL
#define ORCA_SDRAM_SIZE          (32u * 1024u * 1024u)

/*
 * Разметка SDRAM. Значения ориентировочные, подгоняются под реальные
 * разрешения LCD1/LCD2 и объём, который в реальности потребуется GUI.
 */
#define ORCA_FB_POOL_SIZE        (4u * 1024u * 1024u)   /* framebuffers (double buffer) */
#define ORCA_GUI_HEAP_SIZE       (2u * 1024u * 1024u)   /* куча GUI-движка */
#define ORCA_APP_ARENA_SIZE      (ORCA_SDRAM_SIZE - ORCA_FB_POOL_SIZE - ORCA_GUI_HEAP_SIZE)

#define ORCA_FB_POOL_BASE        (ORCA_SDRAM_BASE)
#define ORCA_GUI_HEAP_BASE       (ORCA_FB_POOL_BASE + ORCA_FB_POOL_SIZE)
#define ORCA_APP_ARENA_BASE      (ORCA_GUI_HEAP_BASE + ORCA_GUI_HEAP_SIZE)

/*
 * FB_POOL нарезан на слоты, потому что источников кадров больше одного: GUI
 * (LVGL) и тестовый генератор fbtest. Пока оба писали в ORCA_FB_POOL_BASE,
 * включение обоих сразу означало бы два продюсера в одном буфере.
 *
 * 1 МБ на слот при кадре 480x320x2 = 300 КБ — запас на панель побольше и на
 * выравнивание. Слот 3 зарезервирован под второй буфер LTDC (двойная
 * буферизация), когда появится дисплей.
 *
 * ВАЖНО: ORCA_GUI_HEAP_BASE продублирован в Core/Inc/lv_conf.h как
 * LV_MEM_ADR — препроцессор LVGL не видит этот заголовок. Меняешь разметку —
 * правишь оба места.
 */
#define ORCA_FB_SLOT_SIZE        (1u * 1024u * 1024u)
#define ORCA_FB_SLOT_COUNT       (ORCA_FB_POOL_SIZE / ORCA_FB_SLOT_SIZE)
#define ORCA_FB_SLOT_BASE(n)     (ORCA_FB_POOL_BASE + (uint32_t)(n) * ORCA_FB_SLOT_SIZE)

#define ORCA_FB_SLOT_GUI         0u   /* кадр LVGL */
#define ORCA_FB_SLOT_TEST        1u   /* тестовая картинка fbtest */
/*
 * Слот 2 — снимок кадра GUI для трансляции. Отправка кадра в UART занимает
 * почти секунду, и всё это время LVGL продолжает рисовать в слоте 0: без
 * копии клиент получал бы кадр, у которого верх от одной картинки, а низ от
 * другой. Копию делает orca_lvgl (см. orca_lvgl.h).
 */
#define ORCA_FB_SLOT_GUI_TX      2u

/* Лимиты на один загруженный модуль/приложение в арене (можно тюнить в runtime) */
#define ORCA_APP_MAX_IMAGE_SIZE  (512u * 1024u)
#define ORCA_APP_DEFAULT_STACK   (16u * 1024u)
#define ORCA_APP_MAX_INSTANCES   16u

#endif /* ORCA_MEMMAP_H */

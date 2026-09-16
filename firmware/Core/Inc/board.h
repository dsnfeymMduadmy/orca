#ifndef BOARD_H
#define BOARD_H

#include "stm32h7xx_hal.h"
#include <stdbool.h>

void board_init(void);
void board_led_set(uint8_t led, bool on);
bool board_button_pressed(uint8_t btn);
bool board_sdram_init(void);
bool board_sdram_ready(void);
bool board_qspi_init(void);
/*
 * Почему QSPI не поднялся: стадия отказа и ответ чипа на JEDEC ID (0x9F).
 * Пустая стадия — всё в порядке. Ответ 00 00 00 или FF FF FF значит, что на
 * линиях данных никого нет, и разбираться надо с платой, а не с настройками.
 */
const char*    board_qspi_error(void);
const uint8_t* board_qspi_jedec_id(void);
bool board_sdmmc_init(void);
/*
 * Карта на месте и том 0: смонтирован. ГУИ читает с карты свои страницы,
 * и «карты нет» надо отличать от «карта есть, но страниц на ней нет».
 */
bool board_sdmmc_ready(void);

/*
 * Часы реального времени (LSE, board_rtc.c). Наружу — unix-секунды UTC:
 * раскладывать их в календарь умеет board_rtc_split, чтобы GUI не считал
 * дни и месяцы сам.
 */
typedef struct {
    uint16_t year;
    uint8_t  month;      /* 1..12 */
    uint8_t  day;        /* 1..31 */
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday;    /* 0 = воскресенье */
} orca_datetime_t;

bool     board_rtc_init(void);
bool     board_rtc_ready(void);
/* false — время никто не выставлял, часы идут от заводской даты. */
bool     board_rtc_time_valid(void);
uint64_t board_rtc_unix(void);
bool     board_rtc_set_unix(uint64_t unix_utc);
/* Смещение локального времени от UTC в минутах — только для отображения. */
int32_t  board_rtc_tz_offset(void);
bool     board_rtc_set_tz_offset(int32_t minutes);
void     board_rtc_split(uint64_t unix_time, orca_datetime_t* out);
/* Обратное к split; поле weekday не читается — день недели считается сам. */
uint64_t board_rtc_make_unix(const orca_datetime_t* dt);

#endif

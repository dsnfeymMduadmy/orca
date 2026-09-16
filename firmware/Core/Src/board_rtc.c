#include "board.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * RTC на LSE (32.768 кГц, кварц PC14/PC15). Источник времени для часов
 * на главном экране и для api->system->unix_time().
 *
 * Наружу время отдаётся unix-секундами UTC, а не календарём: с числом
 * проще работать всем — GUI сам разложит его на часы/минуты, а модулю
 * не нужно знать про RTC_TimeTypeDef.
 *
 * Смещение часового пояса лежит в backup-регистре, а не в календаре:
 * в RTC пишем UTC. Иначе после смены пояса пришлось бы переставлять часы,
 * и любая метка времени в логах становилась бы непонятно в чьём поясе.
 *
 * Backup-домен переживает reset и (при батарейке на VBAT) снятие питания,
 * поэтому HAL_RTC_Init на каждом старте — это только перенастройка
 * предделителей: календарь не сбрасывается. Признак «время выставлено» —
 * магия в BKP0R: без неё часы показывают время от сборки прошивки, а не
 * мусор из невыставленного RTC.
 */

#define RTC_MAGIC        0x4F524341u   /* 'ORCA' */
#define RTC_BKP_MAGIC    RTC_BKP_DR0
#define RTC_BKP_TZ       RTC_BKP_DR1

/*
 * Время, с которого идут часы, пока их никто не выставил. Держим не 0
 * (1970), а осмысленную дату: на экране сразу видно и дату, и что она
 * «заводская», а не «часы стоят».
 */
#define RTC_DEFAULT_UNIX  1735689600ull   /* 2025-01-01 00:00:00 UTC */

static RTC_HandleTypeDef s_hrtc;
static bool              s_ready;

void HAL_RTC_MspInit(RTC_HandleTypeDef* hrtc)
{
    (void)hrtc;
    /*
     * Доступ к backup-домену закрыт по сбросу: без DBP запись в календарь и
     * в BKPxR молча не проходит. Источник (LSE) уже выбран в
     * SystemClock_Config через RCC_PERIPHCLK_RTC.
     */
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_RTC_ENABLE();       /* тактирование самого RTC (BDCR.RTCEN) */
    __HAL_RCC_RTC_CLK_ENABLE();   /* APB-интерфейс регистров RTC          */
}

/* ------------------------------------------------------------------ */
/* Календарь <-> unix                                                 */
/* ------------------------------------------------------------------ */
/*
 * Алгоритм days_from_civil/civil_from_days (Howard Hinnant): целочисленный,
 * без таблиц и без деления на 400 в цикле. Обычный «пройти по годам от 1970»
 * тоже работает, но здесь это делается в каждом кадре GUI — считаем сразу
 * за O(1).
 */
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= (m <= 2u) ? 1 : 0;
    int32_t era = ((y >= 0) ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);                     /* 0..399 */
    uint32_t doy = (153u * (m + ((m > 2u) ? -3u : 9u)) + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int32_t* y, uint32_t* m, uint32_t* d)
{
    z += 719468;
    int32_t era = ((z >= 0) ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);                  /* 0..146096 */
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int32_t yy = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + ((mp < 10u) ? 3u : (uint32_t)-9);
    *y = yy + ((*m <= 2u) ? 1 : 0);
}

/* ------------------------------------------------------------------ */

bool board_rtc_init(void)
{
    s_hrtc.Instance            = RTC;
    s_hrtc.Init.HourFormat     = RTC_HOURFORMAT_24;
    /* 32768 / ((127+1) * (255+1)) = 1 Гц */
    s_hrtc.Init.AsynchPrediv   = 127;
    s_hrtc.Init.SynchPrediv    = 255;
    s_hrtc.Init.OutPut         = RTC_OUTPUT_DISABLE;
    s_hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    s_hrtc.Init.OutPutType     = RTC_OUTPUT_TYPE_OPENDRAIN;
    s_hrtc.Init.OutPutRemap    = RTC_OUTPUT_REMAP_NONE;

    if (HAL_RTC_Init(&s_hrtc) != HAL_OK) {
        return false;
    }
    s_ready = true;

    /*
     * Первый старт (или снятое питание без батарейки на VBAT): календарь
     * лежит в неопределённом состоянии — 0-й год, 0-й месяц. Ставим
     * заводскую дату, иначе разложение в дату даёт бессмысленный результат.
     */
    if (HAL_RTCEx_BKUPRead(&s_hrtc, RTC_BKP_MAGIC) != RTC_MAGIC) {
        board_rtc_set_unix(RTC_DEFAULT_UNIX);
        HAL_RTCEx_BKUPWrite(&s_hrtc, RTC_BKP_MAGIC, 0u);  /* время всё ещё «заводское» */
    }
    return true;
}

bool board_rtc_ready(void)
{
    return s_ready;
}

bool board_rtc_time_valid(void)
{
    return s_ready && (HAL_RTCEx_BKUPRead(&s_hrtc, RTC_BKP_MAGIC) == RTC_MAGIC);
}

uint64_t board_rtc_unix(void)
{
    if (!s_ready) {
        return 0;
    }

    RTC_TimeTypeDef t = {0};
    RTC_DateTypeDef d = {0};

    /*
     * Порядок обязателен: GetTime защёлкивает теневые регистры (RSF), и
     * пока не прочитана дата, они не обновятся. Если читать дату первой,
     * раз в сутки в полночь вернётся вчерашняя дата с сегодняшним временем.
     */
    if (HAL_RTC_GetTime(&s_hrtc, &t, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_GetDate(&s_hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) {
        return 0;
    }

    int32_t days = days_from_civil(2000 + (int32_t)d.Year, d.Month, d.Date);
    return (uint64_t)((int64_t)days * 86400ll) +
           t.Hours * 3600ull + t.Minutes * 60ull + t.Seconds;
}

bool board_rtc_set_unix(uint64_t unix_utc)
{
    if (!s_ready) {
        return false;
    }

    int32_t  days = (int32_t)(unix_utc / 86400ull);
    uint32_t rem  = (uint32_t)(unix_utc % 86400ull);

    int32_t  year = 1970;
    uint32_t mon = 1, day = 1;
    civil_from_days(days, &year, &mon, &day);

    /* RTC хранит год двумя цифрами от 2000 — до 2099 включительно. */
    if (year < 2000 || year > 2099) {
        return false;
    }

    RTC_DateTypeDef d = {0};
    d.Year  = (uint8_t)(year - 2000);
    d.Month = (uint8_t)mon;
    d.Date  = (uint8_t)day;
    /* День недели RTC не вычисляет сам, а неверный WeekDay HAL отвергает. */
    d.WeekDay = (uint8_t)(((days % 7) + 10) % 7 + 1);   /* 1970-01-01 — четверг */

    RTC_TimeTypeDef t = {0};
    t.Hours   = (uint8_t)(rem / 3600u);
    t.Minutes = (uint8_t)((rem % 3600u) / 60u);
    t.Seconds = (uint8_t)(rem % 60u);
    t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    t.StoreOperation = RTC_STOREOPERATION_RESET;

    if (HAL_RTC_SetDate(&s_hrtc, &d, RTC_FORMAT_BIN) != HAL_OK ||
        HAL_RTC_SetTime(&s_hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) {
        return false;
    }

    HAL_RTCEx_BKUPWrite(&s_hrtc, RTC_BKP_MAGIC, RTC_MAGIC);
    return true;
}

int32_t board_rtc_tz_offset(void)
{
    if (!s_ready) {
        return 0;
    }
    /* Хранится со сдвигом: в BKPxR только uint32, а пояс бывает отрицательным. */
    int32_t raw = (int32_t)HAL_RTCEx_BKUPRead(&s_hrtc, RTC_BKP_TZ) - 1440;
    return (raw >= -1440 && raw <= 1440) ? raw : 0;
}

bool board_rtc_set_tz_offset(int32_t minutes)
{
    if (!s_ready || minutes < -1440 || minutes > 1440) {
        return false;
    }
    HAL_RTCEx_BKUPWrite(&s_hrtc, RTC_BKP_TZ, (uint32_t)(minutes + 1440));
    return true;
}

void board_rtc_split(uint64_t unix_time, orca_datetime_t* out)
{
    if (out == NULL) {
        return;
    }

    int32_t  days = (int32_t)(unix_time / 86400ull);
    uint32_t rem  = (uint32_t)(unix_time % 86400ull);
    int32_t  year = 1970;
    uint32_t mon = 1, day = 1;

    civil_from_days(days, &year, &mon, &day);

    out->year    = (uint16_t)year;
    out->month   = (uint8_t)mon;
    out->day     = (uint8_t)day;
    out->hour    = (uint8_t)(rem / 3600u);
    out->minute  = (uint8_t)((rem % 3600u) / 60u);
    out->second  = (uint8_t)(rem % 60u);
    /* 1970-01-01 — четверг, поэтому +4 до воскресенья как 0. */
    out->weekday = (uint8_t)(((days % 7) + 11) % 7);
}

uint64_t board_rtc_make_unix(const orca_datetime_t* dt)
{
    if (dt == NULL || dt->month < 1u || dt->month > 12u ||
        dt->day < 1u || dt->day > 31u || dt->hour > 23u ||
        dt->minute > 59u || dt->second > 59u || dt->year < 1970u) {
        return 0;
    }

    int32_t days = days_from_civil((int32_t)dt->year, dt->month, dt->day);
    return (uint64_t)((int64_t)days * 86400ll) +
           dt->hour * 3600ull + dt->minute * 60ull + dt->second;
}

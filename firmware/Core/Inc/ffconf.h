#ifndef FFCONF_H
#define FFCONF_H

/*
 * FatFs конфигурация для Orca OS.
 * Том "0:" — microSD через SDMMC1, том "1:" — QSPI-Flash 16 МБ.
 * Оба заведены в Core/Src/bsp_diskio.c.
 *
 * exFAT включён: карты >32 ГБ приезжают отформатированными в exFAT, и
 * заставлять пользователя переформатировать в FAT32 — плохой UX.
 */

#define FFCONF_DEF 80286           /* Должен совпадать с FF_DEFINED в ff.h (R0.15) */

/* ------------------------------------------------------------------ */
/* Function configuration                                              */
/* ------------------------------------------------------------------ */

#define FF_FS_MINIMIZE    0        /* Нужны f_stat/f_unlink/f_rename/opendir */
#define FF_USE_STRFUNC    0        /* f_gets/f_printf не используем */
#define FF_PRINT_LLI      0
#define FF_PRINT_FLOAT    0
#define FF_USE_FIND       0
#define FF_USE_TRIM       0        /* diskio SDMMC не реализует CTRL_TRIM */

/* ------------------------------------------------------------------ */
/* Platform                                                            */
/* ------------------------------------------------------------------ */

#define FF_FS_TINY        0
#define FF_FS_EXFAT       1
#define FF_FS_NORTC       0        /* get_fattime() реализован */

/* Поддержка времени mtime записи (нужен RTC + get_fattime). */
#define FF_FS_READONLY    0
#define FF_FS_LOCK        0        /* Блокировки на уровне VFS не нужны */

/* LFN: в буфере на стеке (экономит static-память). */
#define FF_USE_LFN        1        /* 1 = буфер LFN на стеке */
#define FF_MAX_LFN        255
#define FF_LFN_UNICODE    2        /* API работает в UTF-8 (TCHAR = char) */
#define FF_LFN_BUF        255
#define FF_SFN_BUF        12

#define FF_STRF_ENCODE    3        /* UTF-8 во всех путях */
#define FF_FS_RPATH       2

/* Кодовая страница OEM для коротких имён; 932/936/… не нужны, берём 437. */
#define FF_CODE_PAGE      437

/* ------------------------------------------------------------------ */
/* Volume / Drive                                                      */
/* ------------------------------------------------------------------ */

#define FF_VOLUMES        2        /* 0: microSD, 1: QSPI */
#define FF_STR_VOLUME_ID  0
#define FF_VOLUME_STRS    "SD","QSPI"
#define FF_MULTI_PARTITION 0

#define FF_MIN_SS         512
#define FF_MAX_SS         512

#define FF_LBA64          0        /* microSD < 2 ТБ */
#define FF_MIN_GPT        0x100

/* ------------------------------------------------------------------ */
/* Оптимизации                                                         */
/* ------------------------------------------------------------------ */

#define FF_USE_FASTSEEK   1
#define FF_USE_EXPAND     0        /* f_expand не используется */
#define FF_USE_CHMOD      0
#define FF_USE_LABEL      1        /* f_getlabel: метка тома в df */
#define FF_USE_FORWARD    0
/*
 * f_mkfs нужен ровно для одного случая: чистый QSPI-чип на новой плате
 * (bsp_qspi_mount). Карты по-прежнему форматируются на ПК.
 */
#define FF_USE_MKFS       1

/* ------------------------------------------------------------------ */
/* Reentrancy (FreeRTOS)                                               */
/* ------------------------------------------------------------------ */

/*
 * Мьютекс пока не нужен: shell и код модуля исполняются последовательно
 * (один модуль в один момент), параллельного доступа к ФС нет.
 * Когда добавится конкурентная запись из GUI + модуля — включить и
 * написать ffsystem.c с FreeRTOS-семафорами под FF_SYNC_t.
 */
#define FF_FS_REENTRANT   0
#define FF_FS_TIMEOUT     1000
#define FF_SYNC_t         void*

/* ------------------------------------------------------------------ */
/* Прочее                                                              */
/* ------------------------------------------------------------------ */

#define FF_FS_NOFSINFO    0        /* Доверяем FSINFO: f_getfree мгновенный */

/* Заглушка часов на случай FF_FS_NORTC=1 (сейчас не используется). */
#define FF_NORTC_MON      1
#define FF_NORTC_MDAY     1
#define FF_NORTC_YEAR     2025

#endif /* FFCONF_H */

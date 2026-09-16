#ifndef ORCA_SHELL_H
#define ORCA_SHELL_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Системная консоль Orca поверх UART (CH340). Однопоточная, вызывается
 * из своего таска: orca_shell_task() блокируется на чтении строки.
 *
 * Транспорт абстрагирован через orca_shell_io_t, чтобы shell работал
 * и на UART, и поверх Orca Link (команды с ПК/Android приходят как
 * ORCA_LINK_CMD_SHELL).
 */

typedef struct {
    int  (*read)(uint8_t* buf, uint32_t len, uint32_t timeout_ms);
    int  (*write)(const uint8_t* buf, uint32_t len);
} orca_shell_io_t;

#define SHELL_LINE_MAX   128

/*
 * Буфер вывода одной команды. 2 КБ, потому что `ls` большого каталога и
 * `help` со списком команд с карты в 1 КБ уже не помещались, а обрезка
 * шла молча. Ответ уходит клиенту по частям (см. orca_link_service.c).
 */
#define SHELL_OUT_MAX    2048

void orca_shell_init(const orca_shell_io_t* io);

/* Баннер + первый промпт. Вызывается владельцем RX при старте. */
void orca_shell_greet(void);

/*
 * Скармливает shell'у один символ (эхо, backspace, запуск по Enter).
 *
 * Нужен потому, что на UART сидят два потребителя — текстовый shell
 * и бинарный Orca Link. Два таска не могут читать один порт (байты
 * растаскаются случайным образом), поэтому RX владеет служба Link,
 * а текстовые байты отдаёт сюда. См. orca_link_service.c.
 */
void orca_shell_feed_byte(uint8_t ch);

/* Бесконечный REPL — только если shell единственный владелец порта. */
void orca_shell_task(void* arg);

/*
 * Разовое выполнение строки команды. Результат пишется в out_buf
 * (NUL-terminated, обрезается по out_len). Возвращает 0 при успехе.
 * Используется и REPL'ом, и мостом Orca Link.
 */
int orca_shell_exec(const char* line, char* out_buf, uint32_t out_len);

/*
 * Буфер вывода текущей команды. Нужен командам с SD-карты: их вывод должен
 * вернуться тому, кто набрал строку, а не уйти в общий printf. Через эти
 * две функции реализован orca_console_api_t (см. orca_api_table.c).
 *
 * Вызывать только изнутри выполняемой команды — буфер один на shell.
 */
void orca_shell_out_write(const char* s);
void orca_shell_out_printf(const char* fmt, ...);

#endif /* ORCA_SHELL_H */

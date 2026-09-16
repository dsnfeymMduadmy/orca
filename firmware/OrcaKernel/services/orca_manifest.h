#ifndef ORCA_MANIFEST_H
#define ORCA_MANIFEST_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Манифест модуля/приложения — файл module.json (для 0:/modules) или
 * app.json (для 0:/apps), лежащий рядом с .orca.
 *
 * Разбор свой, а не готовая JSON-библиотека: манифест плоский (объект из
 * строк, чисел и одного массива строк), и тащить ради него парсер с
 * динамической памятью в ядро незачем. Ограничения осознанные и жёсткие:
 *   - только объект верхнего уровня, вложенные объекты пропускаются;
 *   - никаких \u-escape'ов, из экранирования понимаем \" \\ \/ \n \r \t;
 *   - неизвестные ключи молча игнорируются (манифест должен переживать
 *     появление новых полей в будущих версиях ядра).
 *
 * Всё складывается в POD-структуру фиксированного размера: её можно
 * держать в статическом массиве каталога и копировать без владения.
 */

#define ORCA_MANIFEST_NAME_MAX    32u
#define ORCA_MANIFEST_VERSION_MAX 16u
#define ORCA_MANIFEST_ENTRY_MAX   64u
#define ORCA_MANIFEST_AUTHOR_MAX  32u
#define ORCA_MANIFEST_DESCR_MAX   96u
#define ORCA_MANIFEST_ICON_MAX    32u

/*
 * Предельный размер файла манифеста. Читаем его целиком в буфер на стеке
 * вызывающего, поэтому число маленькое и проверяется явно: манифест на
 * 100 КБ — это не манифест, а чужой файл с тем же именем.
 */
#define ORCA_MANIFEST_FILE_MAX    1024u

/*
 * Права доступа к группам API. Битовая маска, потому что проверка идёт на
 * каждом вызове api->uart->open и подобных — там нужен один тест бита.
 *
 * Имена в JSON совпадают с именами подтаблиц orca_api_t: "uart", "gpio",
 * "spi", "i2c", "storage", "gui", "ipc", "system".
 *
 * Групп log/task/console в списке нет намеренно: без лога модуль не сможет
 * сообщить об отказе, а без task — уснуть и увидеть запрос остановки.
 * Запрещать их означало бы запрещать корректное завершение.
 */
typedef enum {
    ORCA_PERM_NONE    = 0u,
    ORCA_PERM_STORAGE = 1u << 0,
    ORCA_PERM_GPIO    = 1u << 1,
    ORCA_PERM_UART    = 1u << 2,
    ORCA_PERM_SPI     = 1u << 3,
    ORCA_PERM_I2C     = 1u << 4,
    ORCA_PERM_GUI     = 1u << 5,
    ORCA_PERM_IPC     = 1u << 6,
    ORCA_PERM_SYSTEM  = 1u << 7
} orca_perm_t;

/* Все права разом — то, что получает модуль без манифеста (см. ниже). */
#define ORCA_PERM_ALL 0xFFu

typedef enum {
    ORCA_MOD_TYPE_UNKNOWN = 0,
    ORCA_MOD_TYPE_APP,      /* "app"     — есть значок и запись в лаунчере */
    ORCA_MOD_TYPE_MODULE,   /* "module"  — фоновая служба */
    ORCA_MOD_TYPE_COMMAND   /* "command" — команда shell'а, orca_cmd_main */
} orca_module_type_t;

typedef struct {
    char     name[ORCA_MANIFEST_NAME_MAX];
    char     version[ORCA_MANIFEST_VERSION_MAX];
    char     entry[ORCA_MANIFEST_ENTRY_MAX];   /* имя .orca рядом с манифестом */
    char     author[ORCA_MANIFEST_AUTHOR_MAX];
    char     description[ORCA_MANIFEST_DESCR_MAX];
    char     icon[ORCA_MANIFEST_ICON_MAX];

    orca_module_type_t type;
    uint32_t api_version;   /* 0 — поле отсутствовало */
    uint32_t stack;         /* байты; 0 — взять ORCA_APP_DEFAULT_STACK */
    uint32_t permissions;   /* маска orca_perm_t */

    /*
     * CRC32 (та же полиномиальная схема, что у zlib) образа .orca.
     * Поле "crc32" ставит tools/elf2orca. Ноль означает "не считали" —
     * такой модуль грузится, но об этом говорится в логе: иначе переход
     * на подписанные модули ломал бы все уже лежащие на картах сборки.
     */
    uint32_t crc32;
    bool     has_crc32;

    /*
     * Был ли манифест вообще. Модуль без манифеста — это .orca, бросённый
     * в каталог руками; он работает, но получает ORCA_PERM_ALL, потому что
     * ограничивать нечем.
     */
    bool     present;
} orca_manifest_t;

/* Значения по умолчанию: без манифеста — все права, тип неизвестен. */
void orca_manifest_default(orca_manifest_t* out);

/*
 * Разбирает манифест из буфера в памяти. `json` не обязан быть
 * NUL-терминированным — длина берётся из `len`.
 * Возвращает false только на синтаксически негодном входе (не объект).
 */
bool orca_manifest_parse(const char* json, uint32_t len, orca_manifest_t* out);

/*
 * Читает и разбирает манифест по пути VFS. Если файла нет, заполняет
 * out значениями по умолчанию (present = false) и возвращает false.
 */
bool orca_manifest_load(const char* path, orca_manifest_t* out);

/*
 * Имя манифеста, который ожидается рядом с .orca данного типа:
 * "app.json" для приложений, иначе "module.json".
 */
const char* orca_manifest_filename(orca_module_type_t type);

/* Человекочитаемое имя типа — для `apps` и GUI. */
const char* orca_manifest_type_name(orca_module_type_t type);

/*
 * Печатает маску прав в буфер как "uart,gpio" (или "-" для пустой маски,
 * "all" для полной). Возвращает buf.
 */
const char* orca_manifest_perm_str(uint32_t mask, char* buf, uint32_t buf_len);

/* Имя одной группы прав по её биту; NULL, если бит не наш. */
const char* orca_perm_name(orca_perm_t perm);

/*
 * CRC32 (zlib/PKZIP: полином 0xEDB88320, начальное и конечное инвертирование).
 * Считается потоково, чтобы не держать образ .orca в памяти целиком:
 *   uint32_t c = orca_crc32(0, NULL, 0);   // == 0
 *   c = orca_crc32(c, chunk, n);           // на каждый кусок
 */
uint32_t orca_crc32(uint32_t crc, const void* data, uint32_t len);

/* CRC32 файла по пути VFS. false — файл не открылся/не дочитался. */
bool orca_crc32_file(const char* path, uint32_t* out_crc);

#endif /* ORCA_MANIFEST_H */

#include "orca_manifest.h"
#include "orca_fileio.h"
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* CRC32                                                              */
/* ------------------------------------------------------------------ */

/*
 * Таблица на полубайт, а не на байт: 16 слов против 256. Разница в 960
 * байт rodata заметна на фоне того, что CRC здесь считается один раз при
 * загрузке модуля, а не в горячем цикле. Два просмотра таблицы на байт
 * всё равно на порядок быстрее побитового варианта.
 */
static const uint32_t s_crc_nib[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

uint32_t orca_crc32(uint32_t crc, const void* data, uint32_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    if (p == NULL) {
        return crc;
    }

    /*
     * Наружу отдаётся уже инвертированное значение (как у zlib), поэтому
     * на входе разворачиваем обратно: так вызывающий может начать с 0,
     * а не с 0xFFFFFFFF, и продолжать с любого промежуточного результата.
     */
    uint32_t c = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        c ^= p[i];
        c = s_crc_nib[c & 0x0Fu] ^ (c >> 4);
        c = s_crc_nib[c & 0x0Fu] ^ (c >> 4);
    }
    return ~c;
}

bool orca_crc32_file(const char* path, uint32_t* out_crc)
{
    if (path == NULL || out_crc == NULL) {
        return false;
    }

    orca_file_t* f = orca_fileio_open(path, false);
    if (f == NULL) {
        return false;
    }

    /*
     * 512 байт на стеке вызывающего. Больше брать нельзя: CRC считается в
     * задаче shell'а/link, а у неё стек делят ещё FatFs с LFN и разбор ELF.
     */
    uint8_t  buf[512];
    uint32_t crc = 0;
    bool     ok  = true;

    for (;;) {
        int32_t n = orca_fileio_read(f, buf, sizeof(buf));
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        crc = orca_crc32(crc, buf, (uint32_t)n);
    }

    orca_fileio_close(f);
    if (ok) {
        *out_crc = crc;
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Права                                                              */
/* ------------------------------------------------------------------ */

static const struct {
    const char* name;
    uint32_t    bit;
} s_perms[] = {
    { "storage", ORCA_PERM_STORAGE },
    { "gpio",    ORCA_PERM_GPIO    },
    { "uart",    ORCA_PERM_UART    },
    { "spi",     ORCA_PERM_SPI     },
    { "i2c",     ORCA_PERM_I2C     },
    { "gui",     ORCA_PERM_GUI     },
    { "ipc",     ORCA_PERM_IPC     },
    { "system",  ORCA_PERM_SYSTEM  },
};

#define PERM_COUNT (sizeof(s_perms) / sizeof(s_perms[0]))

const char* orca_perm_name(orca_perm_t perm)
{
    for (uint32_t i = 0; i < PERM_COUNT; i++) {
        if (s_perms[i].bit == (uint32_t)perm) {
            return s_perms[i].name;
        }
    }
    return NULL;
}

static uint32_t perm_bit_by_name(const char* name)
{
    for (uint32_t i = 0; i < PERM_COUNT; i++) {
        if (strcmp(s_perms[i].name, name) == 0) {
            return s_perms[i].bit;
        }
    }
    return 0u; /* неизвестное право = никакого; см. комментарий в parse */
}

const char* orca_manifest_perm_str(uint32_t mask, char* buf, uint32_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return "";
    }
    buf[0] = '\0';

    if ((mask & ORCA_PERM_ALL) == ORCA_PERM_ALL) {
        strncpy(buf, "all", buf_len - 1);
        buf[buf_len - 1] = '\0';
        return buf;
    }
    if (mask == 0u) {
        strncpy(buf, "-", buf_len - 1);
        buf[buf_len - 1] = '\0';
        return buf;
    }

    uint32_t pos = 0;
    for (uint32_t i = 0; i < PERM_COUNT; i++) {
        if ((mask & s_perms[i].bit) == 0u) {
            continue;
        }
        uint32_t need = (uint32_t)strlen(s_perms[i].name) + (pos ? 1u : 0u);
        if (pos + need >= buf_len) {
            break;
        }
        if (pos) {
            buf[pos++] = ',';
        }
        memcpy(&buf[pos], s_perms[i].name, strlen(s_perms[i].name));
        pos += (uint32_t)strlen(s_perms[i].name);
    }
    buf[pos] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* Минимальный сканер JSON                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* p;
    const char* end;
} js_t;

static void js_ws(js_t* j)
{
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            j->p++;
        } else if (c == '/' && (j->p + 1) < j->end && j->p[1] == '/') {
            /*
             * Комментарии в JSON недопустимы, но манифест правят руками, и
             * «// TODO» в нём встречается чаще, чем хотелось бы. Дешевле
             * пропустить строку, чем отвергнуть весь файл.
             */
            while (j->p < j->end && *j->p != '\n') {
                j->p++;
            }
        } else {
            break;
        }
    }
}

static bool js_eat(js_t* j, char c)
{
    js_ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return true;
    }
    return false;
}

/*
 * Читает строку в out (усечение до out_len-1 — не ошибка: поля манифеста
 * имеют жёсткие пределы, и слишком длинное описание не повод не запустить
 * модуль). out может быть NULL — тогда строка просто проглатывается.
 */
static bool js_string(js_t* j, char* out, uint32_t out_len)
{
    js_ws(j);
    if (j->p >= j->end || *j->p != '"') {
        return false;
    }
    j->p++;

    uint32_t pos = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u':
                    /*
                     * \uXXXX честно распаковать некуда: поля манифеста —
                     * байтовые строки, а не UTF-16. Пропускаем четыре цифры и
                     * ставим '?', чтобы длина строки осталась осмысленной.
                     */
                    for (uint32_t k = 0; k < 4 && j->p < j->end; k++) {
                        j->p++;
                    }
                    c = '?';
                    break;
                default:  c = e;    break; /* \" \\ \/ и всё прочее — как есть */
            }
        }
        if (out != NULL && pos + 1 < out_len) {
            out[pos++] = c;
        }
    }

    if (j->p >= j->end) {
        return false; /* незакрытая кавычка */
    }
    j->p++; /* закрывающая " */

    if (out != NULL && out_len > 0) {
        out[pos] = '\0';
    }
    return true;
}

static bool js_number(js_t* j, uint32_t* out)
{
    js_ws(j);
    const char* start = j->p;
    while (j->p < j->end) {
        char c = *j->p;
        bool numeric = (c >= '0' && c <= '9') || c == 'x' || c == 'X' ||
                       (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
                       c == '+' || c == '-';
        if (!numeric) {
            break;
        }
        j->p++;
    }
    if (j->p == start) {
        return false;
    }

    char tmp[24];
    uint32_t n = (uint32_t)(j->p - start);
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, start, n);
    tmp[n] = '\0';

    if (out != NULL) {
        *out = (uint32_t)strtoul(tmp, NULL, 0);
    }
    return true;
}

/* Пропускает произвольное значение, включая вложенные объекты и массивы. */
static bool js_skip_value(js_t* j)
{
    js_ws(j);
    if (j->p >= j->end) {
        return false;
    }

    char c = *j->p;
    if (c == '"') {
        return js_string(j, NULL, 0);
    }
    if (c == '{' || c == '[') {
        char open  = c;
        char close = (c == '{') ? '}' : ']';
        int  depth = 0;
        while (j->p < j->end) {
            char k = *j->p;
            if (k == '"') {
                /* Строку пропускаем целиком: скобка внутри неё не считается. */
                if (!js_string(j, NULL, 0)) {
                    return false;
                }
                continue;
            }
            j->p++;
            if (k == open) {
                depth++;
            } else if (k == close) {
                if (--depth == 0) {
                    return true;
                }
            }
        }
        return false;
    }

    /* Число, true/false/null — просто до разделителя. */
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']') {
        j->p++;
    }
    return true;
}

/* Массив строк -> маска прав. */
static bool js_perm_array(js_t* j, uint32_t* mask)
{
    if (!js_eat(j, '[')) {
        return false;
    }
    *mask = 0u;

    js_ws(j);
    if (js_eat(j, ']')) {
        return true; /* "permissions": [] — модуль без прав, это законно */
    }

    for (;;) {
        char name[16];
        if (!js_string(j, name, sizeof(name))) {
            return false;
        }
        /*
         * Неизвестное имя права не роняет разбор и не даёт доступа: так
         * манифест, написанный под более новое ядро, остаётся рабочим —
         * просто без той группы, которой здесь ещё нет.
         */
        *mask |= perm_bit_by_name(name);

        js_ws(j);
        if (js_eat(j, ',')) {
            continue;
        }
        return js_eat(j, ']');
    }
}

/* ------------------------------------------------------------------ */
/* Разбор манифеста                                                   */
/* ------------------------------------------------------------------ */

void orca_manifest_default(orca_manifest_t* out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    /*
     * Без манифеста ограничивать нечем: .orca, положенный в каталог руками,
     * должен работать так же, как работал до появления прав. Ужесточение
     * здесь молча сломало бы все существующие карты.
     */
    out->permissions = ORCA_PERM_ALL;
    out->type        = ORCA_MOD_TYPE_UNKNOWN;
    out->present     = false;
}

static orca_module_type_t type_from_name(const char* s)
{
    if (strcmp(s, "app") == 0)     return ORCA_MOD_TYPE_APP;
    if (strcmp(s, "module") == 0)  return ORCA_MOD_TYPE_MODULE;
    if (strcmp(s, "command") == 0) return ORCA_MOD_TYPE_COMMAND;
    return ORCA_MOD_TYPE_UNKNOWN;
}

bool orca_manifest_parse(const char* json, uint32_t len, orca_manifest_t* out)
{
    if (json == NULL || out == NULL) {
        return false;
    }

    orca_manifest_default(out);

    js_t j = { json, json + len };
    if (!js_eat(&j, '{')) {
        return false;
    }

    /*
     * Права по умолчанию для манифеста, в котором нет "permissions".
     * Такой манифест описывает модуль, написанный до появления прав, —
     * он получает всё, иначе обновление ядра ломало бы рабочие модули.
     * Наличие ключа переводит модуль в режим белого списка.
     */
    uint32_t perms     = ORCA_PERM_ALL;
    bool     perms_set = false;

    js_ws(&j);
    if (js_eat(&j, '}')) {
        out->present     = true;
        out->permissions = perms;
        return true;
    }

    for (;;) {
        char key[24];
        if (!js_string(&j, key, sizeof(key))) {
            return false;
        }
        if (!js_eat(&j, ':')) {
            return false;
        }

        bool ok = true;
        if (strcmp(key, "name") == 0) {
            ok = js_string(&j, out->name, sizeof(out->name));
        } else if (strcmp(key, "version") == 0) {
            ok = js_string(&j, out->version, sizeof(out->version));
        } else if (strcmp(key, "entry") == 0) {
            ok = js_string(&j, out->entry, sizeof(out->entry));
        } else if (strcmp(key, "author") == 0) {
            ok = js_string(&j, out->author, sizeof(out->author));
        } else if (strcmp(key, "description") == 0) {
            ok = js_string(&j, out->description, sizeof(out->description));
        } else if (strcmp(key, "icon") == 0) {
            ok = js_string(&j, out->icon, sizeof(out->icon));
        } else if (strcmp(key, "type") == 0) {
            char t[16];
            ok = js_string(&j, t, sizeof(t));
            if (ok) {
                out->type = type_from_name(t);
            }
        } else if (strcmp(key, "api_version") == 0) {
            ok = js_number(&j, &out->api_version);
        } else if (strcmp(key, "stack") == 0) {
            ok = js_number(&j, &out->stack);
        } else if (strcmp(key, "crc32") == 0) {
            /*
             * Допускаем и число (0x1234ABCD), и строку ("1234abcd"):
             * elf2orca пишет строку, потому что JSON-числа больше 2^31
             * некоторые редакторы превращают в float и теряют точность.
             */
            js_ws(&j);
            if (j.p < j.end && *j.p == '"') {
                char hex[16];
                ok = js_string(&j, hex, sizeof(hex));
                if (ok) {
                    out->crc32     = (uint32_t)strtoul(hex, NULL, 16);
                    out->has_crc32 = true;
                }
            } else {
                ok = js_number(&j, &out->crc32);
                if (ok) {
                    out->has_crc32 = true;
                }
            }
        } else if (strcmp(key, "permissions") == 0) {
            ok = js_perm_array(&j, &perms);
            if (ok) {
                perms_set = true;
            }
        } else {
            ok = js_skip_value(&j);
        }

        if (!ok) {
            return false;
        }

        js_ws(&j);
        if (js_eat(&j, ',')) {
            continue;
        }
        if (js_eat(&j, '}')) {
            break;
        }
        return false;
    }

    (void)perms_set;
    out->permissions = perms;
    out->present     = true;
    return true;
}

bool orca_manifest_load(const char* path, orca_manifest_t* out)
{
    if (out == NULL) {
        return false;
    }
    orca_manifest_default(out);

    if (path == NULL) {
        return false;
    }

    orca_file_t* f = orca_fileio_open(path, false);
    if (f == NULL) {
        return false;
    }

    uint32_t size = orca_fileio_size(f);
    if (size == 0 || size > ORCA_MANIFEST_FILE_MAX) {
        orca_fileio_close(f);
        return false;
    }

    /*
     * 1 КБ на стеке. Вызывается из задач shell/link, у которых стек 3072
     * слова, — помещается с запасом; в задаче модуля манифест не читают.
     */
    char    buf[ORCA_MANIFEST_FILE_MAX];
    int32_t n = orca_fileio_read(f, buf, size);
    orca_fileio_close(f);

    if (n <= 0) {
        return false;
    }

    return orca_manifest_parse(buf, (uint32_t)n, out);
}

const char* orca_manifest_filename(orca_module_type_t type)
{
    return (type == ORCA_MOD_TYPE_APP) ? "app.json" : "module.json";
}

const char* orca_manifest_type_name(orca_module_type_t type)
{
    switch (type) {
        case ORCA_MOD_TYPE_APP:     return "app";
        case ORCA_MOD_TYPE_MODULE:  return "module";
        case ORCA_MOD_TYPE_COMMAND: return "command";
        default:                    return "?";
    }
}

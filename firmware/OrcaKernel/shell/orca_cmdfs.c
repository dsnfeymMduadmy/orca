#include "orca_cmdfs.h"
#include "orca_loader.h"
#include "orca_api.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>

extern const orca_api_t g_orca_api;

/*
 * Имя команды приходит из строки, набранной пользователем, и подставляется
 * в путь. Разрешаем только [A-Za-z0-9_-]: иначе "cd ../../system" или
 * "0:/secrets" превращают безобидный dispatch в чтение произвольного файла
 * (и запуск произвольного кода — команда исполняется).
 */
static bool name_is_safe(const char* name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    uint32_t len = 0;
    for (const char* p = name; *p != '\0'; p++, len++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return len <= ORCA_CMDFS_NAME_MAX;
}

orca_cmdfs_status_t orca_cmdfs_exec(int argc, char** argv, int* out_rc)
{
    if (argc < 1 || argv == NULL || argv[0] == NULL) {
        return ORCA_CMDFS_NOT_FOUND;
    }
    if (!name_is_safe(argv[0])) {
        return ORCA_CMDFS_NOT_FOUND;
    }

    char path[ORCA_CMDFS_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s%s",
                     ORCA_CMDFS_DIR, argv[0], ORCA_CMDFS_EXT);
    if (n <= 0 || (uint32_t)n >= sizeof(path)) {
        return ORCA_CMDFS_BAD_NAME;
    }

    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK || (fno.fattrib & AM_DIR)) {
        return ORCA_CMDFS_NOT_FOUND;
    }

    orca_loaded_module_t mod;
    orca_load_status_t st = orca_loader_load(path, &g_orca_api, &mod);
    if (st != ORCA_LOAD_OK) {
        return ORCA_CMDFS_LOAD_ERROR;
    }

    if (mod.cmd_fn == NULL) {
        orca_loader_unload(&mod);
        return ORCA_CMDFS_NO_ENTRY;
    }

    int rc = mod.cmd_fn(&g_orca_api, argc, argv);
    orca_loader_unload(&mod);

    if (out_rc != NULL) {
        *out_rc = rc;
    }
    return ORCA_CMDFS_OK;
}

const char* orca_cmdfs_status_str(orca_cmdfs_status_t st)
{
    switch (st) {
        case ORCA_CMDFS_OK:         return "ok";
        case ORCA_CMDFS_NOT_FOUND:  return "не найдена";
        case ORCA_CMDFS_BAD_NAME:   return "недопустимое имя";
        case ORCA_CMDFS_LOAD_ERROR: return "ошибка загрузки образа";
        case ORCA_CMDFS_NO_ENTRY:   return "нет orca_cmd_main";
        default:                    return "?";
    }
}

uint32_t orca_cmdfs_list(void (*cb)(const char* name, uint32_t size, void* ctx),
                         void* ctx)
{
    DIR dir;
    if (f_opendir(&dir, ORCA_CMDFS_DIR) != FR_OK) {
        return 0;
    }

    const uint32_t ext_len = (uint32_t)strlen(ORCA_CMDFS_EXT);
    uint32_t count = 0;
    FILINFO fno;

    for (;;) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') {
            break;
        }
        if (fno.fattrib & AM_DIR) {
            continue;
        }
        uint32_t len = (uint32_t)strlen(fno.fname);
        if (len <= ext_len || strcmp(&fno.fname[len - ext_len], ORCA_CMDFS_EXT) != 0) {
            continue;
        }

        char name[ORCA_CMDFS_NAME_MAX + 1];
        uint32_t base = len - ext_len;
        if (base > ORCA_CMDFS_NAME_MAX) {
            base = ORCA_CMDFS_NAME_MAX;
        }
        memcpy(name, fno.fname, base);
        name[base] = '\0';

        if (cb != NULL) {
            cb(name, (uint32_t)fno.fsize, ctx);
        }
        count++;
    }

    f_closedir(&dir);
    return count;
}

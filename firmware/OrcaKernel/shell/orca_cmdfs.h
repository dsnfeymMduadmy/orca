#ifndef ORCA_CMDFS_H
#define ORCA_CMDFS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Команды shell'а, живущие на SD-карте: 0:/commands/<имя>.orca — обычный
 * relocatable ELF с точкой входа orca_cmd_main(api, argc, argv).
 *
 * Отличие от orca_appmgr: команда не получает своей задачи и своего стека.
 * Она загружается в арену, выполняется синхронно в задаче того, кто набрал
 * строку, пишет вывод через api->console и выгружается. Так ядро остаётся
 * маленьким, а набор команд обновляется заменой файла на карте.
 */

#define ORCA_CMDFS_DIR       "0:/commands"
#define ORCA_CMDFS_EXT       ".orca"
#define ORCA_CMDFS_NAME_MAX  32u
#define ORCA_CMDFS_PATH_MAX  96u

typedef enum {
    ORCA_CMDFS_OK = 0,        /* команда найдена и выполнена, rc заполнен */
    ORCA_CMDFS_NOT_FOUND,     /* такого файла в 0:/commands нет           */
    ORCA_CMDFS_BAD_NAME,      /* имя не прошло проверку (см. ниже)        */
    ORCA_CMDFS_LOAD_ERROR,    /* файл есть, но не загрузился              */
    ORCA_CMDFS_NO_ENTRY       /* загрузился, но нет orca_cmd_main         */
} orca_cmdfs_status_t;

/*
 * Ищет и выполняет argv[0] как команду с карты. Вывод команды уходит
 * в буфер shell'а (orca_shell_out_*), поэтому вызывать только из того же
 * контекста, что и orca_shell_exec().
 *
 * ORCA_CMDFS_NOT_FOUND — сигнал вызывающему, что имя вообще не команда,
 * и надо печатать "неизвестная команда". Остальные коды — команда есть,
 * но запустить её не удалось.
 */
orca_cmdfs_status_t orca_cmdfs_exec(int argc, char** argv, int* out_rc);

/* Человекочитаемое описание кода ошибки (для сообщений shell'а). */
const char* orca_cmdfs_status_str(orca_cmdfs_status_t st);

/*
 * Перечисляет команды на карте, вызывая cb на каждое имя без расширения.
 * Возвращает число найденных команд (0, если каталога нет).
 */
uint32_t orca_cmdfs_list(void (*cb)(const char* name, uint32_t size, void* ctx),
                         void* ctx);

#endif /* ORCA_CMDFS_H */

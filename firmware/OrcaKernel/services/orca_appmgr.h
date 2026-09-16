#ifndef ORCA_APPMGR_H
#define ORCA_APPMGR_H

#include <stdint.h>
#include <stdbool.h>
#include "orca_loader.h"
#include "orca_manifest.h"
#include "orca_memmap.h"

#define ORCA_APPMGR_MAX_RUNNING ORCA_APP_MAX_INSTANCES

/* Каталоги, которые обходит orca_appmgr_scan(). */
#define ORCA_APPMGR_APPS_DIR    "0:/apps"
#define ORCA_APPMGR_MODULES_DIR "0:/modules"

/* Сколько записей каталога помещается в статический список. */
#define ORCA_APPMGR_CATALOG_MAX 32u

#define ORCA_APPMGR_PATH_MAX 128u

/*
 * Бит запроса остановки в notification value задачи модуля.
 * Его выставляет orca_appmgr_stop(), а модуль видит через
 * api->task->should_stop() (реализация — orca_api_table.c).
 */
#define ORCA_STOP_NOTIFY_BIT 0x01u

/* Сколько ждём добровольного выхода модуля, прежде чем снять задачу. */
#define ORCA_APPMGR_STOP_GRACE_MS 100u

typedef enum {
    ORCA_APP_STATE_STOPPED = 0,
    ORCA_APP_STATE_RUNNING,
    /*
     * Кто-то уже забрал сворачивание инстанса себе (задача модуля вышла сама
     * либо orca_appmgr_stop снимает её принудительно). Состояние нужно,
     * чтобы orca_loader_unload не выполнился дважды: и выход из main_fn, и
     * истёкший grace-период ведут к одному и тому же освобождению арены.
     */
    ORCA_APP_STATE_STOPPING,
    ORCA_APP_STATE_ERROR
} orca_app_state_t;

typedef struct {
    int32_t               id;          /* индекс слота — то, что понимает orca_appmgr_stop */
    char                  name[32];
    char                  path[ORCA_APPMGR_PATH_MAX];
    orca_app_state_t      state;
    orca_loaded_module_t  module;
    void*                 task_handle; /* TaskHandle_t, opaque здесь чтобы не тащить FreeRTOS.h в API */
    uint32_t              start_time_ms;
    /*
     * Права из манифеста. Задача модуля кладёт их в свой TLS при старте,
     * и дальше каждая проверка в orca_api_table.c читает именно их —
     * инстанс для этого искать не нужно.
     */
    uint32_t              permissions;
} orca_app_instance_t;

/*
 * Запись каталога: то, что лежит на карте и может быть запущено.
 * Это не инстанс — здесь ничего не загружено в память.
 */
typedef struct {
    char               path[ORCA_APPMGR_PATH_MAX]; /* полный путь к .orca */
    orca_module_type_t type;      /* из манифеста, иначе по каталогу */
    orca_manifest_t    manifest;  /* present = false, если манифеста нет */
    uint32_t           size;      /* размер образа, байт */
} orca_catalog_entry_t;

/* Инициализация менеджера (сброс списка запущенных инстансов). */
void orca_appmgr_init(const orca_api_t* api);

/*
 * Запускает модуль/приложение по пути VFS (`SD:/modules/x/x.orca` и т.п.)
 * в новой FreeRTOS-задаче. Возвращает индекс инстанса (>=0) или -1 при ошибке.
 */
int32_t orca_appmgr_start(const char* path, uint32_t stack_size);

/* Останавливает запущенный инстанс (вызывает orca_app_stop, затем убивает таск). */
bool orca_appmgr_stop(int32_t instance_id);

/* Список запущенных инстансов, для GUI/shell (`ps`). */
uint32_t orca_appmgr_list(orca_app_instance_t* out, uint32_t max_count);

/* ------------------------------------------------------------------ */
/* Каталог: что лежит на карте                                        */
/* ------------------------------------------------------------------ */

/*
 * Обходит 0:/apps и 0:/modules, разбирает манифесты и пересобирает
 * внутренний каталог. Возвращает число найденных записей.
 *
 * Признаётся и каталог с манифестом (`<dir>/<entry>.orca` + app.json /
 * module.json), и одиночный `<dir>/<имя>.orca`, брошенный на карту руками:
 * второй вариант удобнее при отладке, и отказывать ему только из-за
 * отсутствия манифеста незачем — он просто получает права по умолчанию.
 *
 * Читает каталоги FatFs, поэтому вызывать только из задачи с достаточным
 * стеком (shell/link), не из обработчика прерывания.
 */
uint32_t orca_appmgr_scan(void);

/*
 * Копирует до max_count записей каталога. Если orca_appmgr_scan() ещё не
 * вызывали, сканирует сам. Возвращает число скопированных записей.
 */
uint32_t orca_appmgr_catalog(orca_catalog_entry_t* out, uint32_t max_count);

/*
 * Ищет запись каталога по имени (из манифеста либо по имени каталога).
 * Возвращает false, если такой записи нет.
 */
bool orca_appmgr_find(const char* name, orca_catalog_entry_t* out);

/*
 * Отображаемое имя записи: поле "name" манифеста, а без манифеста —
 * имя каталога/файла без расширения.
 */
void orca_appmgr_entry_name(const orca_catalog_entry_t* e,
                            char* out, uint32_t out_len);

/* ------------------------------------------------------------------ */
/* Права текущей задачи                                               */
/* ------------------------------------------------------------------ */

/*
 * Права хранятся в TLS-слоте задачи (индекс 0). Задачи ядра его не
 * трогают, поэтому у них там NULL — и это означает "без ограничений":
 * shell, GUI и службы обязаны работать поверх той же таблицы API, что и
 * модули, и проверять их права было бы и бессмысленно, и опасно.
 *
 * Ограничен ровно тот, кто явно себя ограничил, — задача модуля в
 * app_task_entry().
 */
void orca_appmgr_task_perm_set(uint32_t mask);

/* Снять ограничение с текущей задачи (вернуть её в режим ядра). */
void orca_appmgr_task_perm_clear(void);

/* Разрешена ли группа API текущей задаче. */
bool orca_appmgr_task_perm_allows(orca_perm_t perm);

/* Маска текущей задачи; ORCA_PERM_ALL, если задача не ограничена. */
uint32_t orca_appmgr_task_perm_mask(void);

#endif /* ORCA_APPMGR_H */

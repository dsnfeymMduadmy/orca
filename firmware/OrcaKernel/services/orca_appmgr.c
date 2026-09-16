#include "orca_appmgr.h"
#include "orca_arena.h"
#include "orca_manifest.h"
#include "orca_api.h"
#include "ff.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

static orca_app_instance_t s_instances[ORCA_APPMGR_MAX_RUNNING];
static const orca_api_t*   s_api;

static orca_catalog_entry_t s_catalog[ORCA_APPMGR_CATALOG_MAX];
static uint32_t             s_catalog_n;
static bool                 s_catalog_valid;

/*
 * TLS-слот, в котором задача модуля держит свою маску прав.
 * configNUM_THREAD_LOCAL_STORAGE_POINTERS в FreeRTOSConfig.h == 2, слоты
 * 0 и 1 свободны; берём нулевой.
 */
#define PERM_TLS_INDEX 0

/*
 * Маска прав кодируется прямо в указателе TLS. Низкие 8 бит — сами права
 * (orca_perm_t), а бит PERM_TAG отличает "явно ограничен" от "слот пуст":
 * NULL в слоте означает задачу ядра без ограничений, а модуль с нулевыми
 * правами хранит PERM_TAG | 0, что тоже не NULL.
 */
#define PERM_TAG 0x100u

/* ------------------------------------------------------------------ */
/* Права текущей задачи                                               */
/* ------------------------------------------------------------------ */

void orca_appmgr_task_perm_set(uint32_t mask)
{
    uintptr_t enc = (uintptr_t)((mask & 0xFFu) | PERM_TAG);
    vTaskSetThreadLocalStoragePointer(NULL, PERM_TLS_INDEX, (void*)enc);
}

void orca_appmgr_task_perm_clear(void)
{
    vTaskSetThreadLocalStoragePointer(NULL, PERM_TLS_INDEX, NULL);
}

uint32_t orca_appmgr_task_perm_mask(void)
{
    /*
     * До запуска планировщика TLS трогать нельзя (нет текущей задачи).
     * Такой вызов означает инициализацию из main() — там ограничений нет.
     */
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        return ORCA_PERM_ALL;
    }
    uintptr_t enc = (uintptr_t)pvTaskGetThreadLocalStoragePointer(NULL, PERM_TLS_INDEX);
    if (enc == 0) {
        return ORCA_PERM_ALL; /* задача ядра — без ограничений */
    }
    return (uint32_t)(enc & 0xFFu);
}

bool orca_appmgr_task_perm_allows(orca_perm_t perm)
{
    return (orca_appmgr_task_perm_mask() & (uint32_t)perm) != 0u;
}

/* ------------------------------------------------------------------ */
/* Жизненный цикл инстанса                                            */
/* ------------------------------------------------------------------ */

/*
 * Право выгрузить инстанс достаётся ровно одному из двух претендентов:
 * задаче модуля (вышла из main_fn) или orca_appmgr_stop (истёк grace).
 * Переход RUNNING -> STOPPING атомарен, поэтому второй претендент видит,
 * что опоздал, и не трогает ни orca_loader_unload, ни vTaskDelete.
 */
static bool claim_teardown(orca_app_instance_t* inst)
{
    bool claimed = false;
    taskENTER_CRITICAL();
    if (inst->state == ORCA_APP_STATE_RUNNING) {
        inst->state = ORCA_APP_STATE_STOPPING;
        claimed = true;
    }
    taskEXIT_CRITICAL();
    return claimed;
}

static void app_task_entry(void* param)
{
    orca_app_instance_t* inst = (orca_app_instance_t*)param;

    /*
     * Права выставляем здесь, а не в orca_appmgr_start: TLS принадлежит
     * задаче, и записать его можно только из неё самой. С этого момента
     * любой вызов inst через таблицу API проходит проверку прав.
     */
    orca_appmgr_task_perm_set(inst->permissions);

    int rc = inst->module.main_fn(s_api);
    (void)rc;

    if (claim_teardown(inst)) {
        orca_loader_unload(&inst->module);
        inst->task_handle = NULL;
        inst->state = ORCA_APP_STATE_STOPPED;
    }
    vTaskDelete(NULL);
}

void orca_appmgr_init(const orca_api_t* api)
{
    s_api = api;
    memset(s_instances, 0, sizeof(s_instances));
    s_catalog_n     = 0;
    s_catalog_valid = false;
    orca_loader_init();
    orca_arena_init();
}

/*
 * Ищет манифест рядом с образом .orca. `orca_path` — полный путь к образу;
 * каталог из него отрезаем и пробуем app.json, затем module.json.
 * Заполняет out значениями по умолчанию, если манифеста нет.
 */
static void manifest_beside(const char* orca_path, orca_manifest_t* out)
{
    orca_manifest_default(out);

    const char* slash = strrchr(orca_path, '/');
    if (slash == NULL) {
        return;
    }
    uint32_t dir_len = (uint32_t)(slash - orca_path);
    if (dir_len == 0 || dir_len >= ORCA_APPMGR_PATH_MAX - 16u) {
        return;
    }

    char base[ORCA_APPMGR_PATH_MAX];
    memcpy(base, orca_path, dir_len);
    base[dir_len] = '\0';

    char path[ORCA_APPMGR_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/app.json", base) > 0 &&
        orca_manifest_load(path, out)) {
        return;
    }
    if (snprintf(path, sizeof(path), "%s/module.json", base) > 0 &&
        orca_manifest_load(path, out)) {
        return;
    }
}

static void log_warn(const char* msg)
{
    if (s_api && s_api->log) {
        s_api->log->warn("appmgr", msg);
    }
}

int32_t orca_appmgr_start(const char* path, uint32_t stack_size)
{
    if (path == NULL) {
        return -1;
    }

    /*
     * Слот берём только полностью свободный. Проверка "не RUNNING" забирала
     * и слот в состоянии STOPPING: memset ниже затирал module, из-под ещё
     * живущей задачи модуля, которая как раз шла к orca_loader_unload.
     */
    int32_t slot = -1;
    for (uint32_t i = 0; i < ORCA_APPMGR_MAX_RUNNING; i++) {
        orca_app_state_t st = s_instances[i].state;
        if (st == ORCA_APP_STATE_STOPPED || st == ORCA_APP_STATE_ERROR) {
            slot = (int32_t)i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    /*
     * Манифест читаем до загрузки образа: он решает, запускать ли модуль
     * вообще (несовпадение CRC или слишком новый api_version), и с какими
     * правами. Отсутствие манифеста — не отказ: тогда права полные, а CRC
     * не проверяется (см. orca_manifest_default).
     */
    orca_manifest_t man;
    manifest_beside(path, &man);

    /*
     * api_version модуля не должен превышать версию ядра: таблица API
     * растёт только добавлением полей в конец, поэтому модуль, собранный
     * под более старую версию, работает и на новой (пользуется подмножеством),
     * а собранный под более новую — обратится к полю, которого здесь нет.
     */
    if (man.present && man.api_version != 0 && man.api_version > ORCA_API_VERSION) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "'%s': api_version %lu новее ядра (%u), запуск отклонён",
                 path, (unsigned long)man.api_version, (unsigned)ORCA_API_VERSION);
        log_warn(msg);
        return -1;
    }

    /*
     * Контрольная сумма. Считаем её только если манифест её объявил: карта
     * со старыми модулями без crc32 обязана остаться рабочей. Несовпадение —
     * повреждённый или подменённый образ, запускать нельзя.
     */
    if (man.has_crc32) {
        uint32_t crc = 0;
        if (!orca_crc32_file(path, &crc)) {
            log_warn("не удалось прочитать образ для проверки CRC");
            return -1;
        }
        if (crc != man.crc32) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "'%s': CRC32 %08lX != %08lX в манифесте",
                     path, (unsigned long)crc, (unsigned long)man.crc32);
            log_warn(msg);
            return -1;
        }
    }

    orca_app_instance_t* inst = &s_instances[slot];
    memset(inst, 0, sizeof(*inst));

    orca_load_status_t status = orca_loader_load(path, s_api, &inst->module);
    if (status != ORCA_LOAD_OK) {
        return -1;
    }

    inst->id = slot;
    inst->permissions = man.permissions;
    strncpy(inst->path, path, sizeof(inst->path) - 1);
    /* Имя из манифеста, если оно есть; иначе то, что нашёл загрузчик. */
    if (man.present && man.name[0] != '\0') {
        strncpy(inst->name, man.name, sizeof(inst->name) - 1);
    } else {
        strncpy(inst->name, inst->module.name, sizeof(inst->name) - 1);
    }

    /* Приоритет размера стека: аргумент вызова -> манифест -> умолчание. */
    uint32_t stack_bytes = stack_size ? stack_size :
                           (man.stack ? man.stack : ORCA_APP_DEFAULT_STACK);
    uint32_t stack_words = stack_bytes / sizeof(StackType_t);
    /*
     * stack_size приходит из module.json, то есть из файла на SD. Значение
     * "stack": 64 давало 16 слов — этого не хватает даже на пролог задачи,
     * и модуль ронял систему по переполнению стека, где настоящая причина
     * (опечатка в манифесте) никак не видна. Клампим снизу и говорим об этом
     * вслух: молчаливая правка чужого числа хуже самой опечатки.
     */
    if (stack_words < configMINIMAL_STACK_SIZE) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "stack %lu words in manifest is below minimum %u, clamped",
                 (unsigned long)stack_words, (unsigned)configMINIMAL_STACK_SIZE);
        log_warn(msg);
        stack_words = configMINIMAL_STACK_SIZE;
    }

    TaskHandle_t handle = NULL;
    BaseType_t ok = xTaskCreate(app_task_entry, inst->name, stack_words, inst,
                                tskIDLE_PRIORITY + 1, &handle);
    if (ok != pdPASS) {
        orca_loader_unload(&inst->module);
        return -1;
    }

    inst->task_handle = handle;
    inst->state = ORCA_APP_STATE_RUNNING;
    inst->start_time_ms = (uint32_t)xTaskGetTickCount();

    return slot;
}

bool orca_appmgr_stop(int32_t instance_id)
{
    if (instance_id < 0 || instance_id >= (int32_t)ORCA_APPMGR_MAX_RUNNING) {
        return false;
    }
    orca_app_instance_t* inst = &s_instances[instance_id];
    if (inst->state != ORCA_APP_STATE_RUNNING) {
        return false;
    }

    /*
     * Сначала просим выйти сами: без этого уведомления should_stop() в модуле
     * всегда возвращал false, и единственным способом остановки было
     * vTaskDelete — с утечкой всего, что модуль успел захватить (файлы,
     * пины, мьютексы).
     */
    if (inst->task_handle != NULL) {
        xTaskNotify((TaskHandle_t)inst->task_handle,
                    ORCA_STOP_NOTIFY_BIT, eSetBits);
    }

    if (inst->module.stop_fn != NULL) {
        inst->module.stop_fn();
    }

    /* Даём модулю догрести до выхода: app_task_entry сам всё выгрузит. */
    for (uint32_t waited = 0; waited < ORCA_APPMGR_STOP_GRACE_MS; waited += 10) {
        if (inst->state != ORCA_APP_STATE_RUNNING) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /*
     * Не вышел — снимаем принудительно, но только если задача модуля не
     * успела начать выгрузку сама: иначе арена освобождалась дважды, а
     * vTaskDelete прилетал в уже удаляющийся таск.
     */
    if (!claim_teardown(inst)) {
        return true;
    }

    TaskHandle_t handle = (TaskHandle_t)inst->task_handle;
    inst->task_handle = NULL;
    if (handle != NULL) {
        vTaskDelete(handle);
    }

    orca_loader_unload(&inst->module);
    inst->state = ORCA_APP_STATE_STOPPED;
    return true;
}

uint32_t orca_appmgr_list(orca_app_instance_t* out, uint32_t max_count)
{
    if (out == NULL) {
        return 0;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < ORCA_APPMGR_MAX_RUNNING && n < max_count; i++) {
        if (s_instances[i].state == ORCA_APP_STATE_RUNNING) {
            out[n] = s_instances[i];
            /* Список сжатый, поэтому id несём с собой: kill ждёт номер слота. */
            out[n].id = (int32_t)i;
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Сканирование каталогов                                            */
/* ------------------------------------------------------------------ */

/* Имя записи для поиска/показа: из манифеста, иначе имя каталога/файла. */
void orca_appmgr_entry_name(const orca_catalog_entry_t* e,
                            char* out, uint32_t out_len)
{
    if (e == NULL || out == NULL || out_len == 0) {
        return;
    }
    if (e->manifest.present && e->manifest.name[0] != '\0') {
        strncpy(out, e->manifest.name, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    /* Фолбэк: последний сегмент пути без ".orca". */
    const char* slash = strrchr(e->path, '/');
    const char* base  = slash ? slash + 1 : e->path;
    uint32_t len = (uint32_t)strlen(base);
    if (len > 5u && strcmp(&base[len - 5], ".orca") == 0) {
        len -= 5u;
    }
    if (len > out_len - 1) {
        len = out_len - 1;
    }
    memcpy(out, base, len);
    out[len] = '\0';
}

/*
 * Добавляет в каталог один подкаталог dir_root/dir_name. Ищет образ .orca
 * в порядке: <entry из манифеста> -> <dir_name>.orca -> app.orca/module.orca.
 * Возвращает true, если запись добавлена.
 */
static bool catalog_add_dir(const char* dir_root, const char* dir_name,
                            orca_module_type_t root_type)
{
    if (s_catalog_n >= ORCA_APPMGR_CATALOG_MAX) {
        return false;
    }

    char sub[ORCA_APPMGR_PATH_MAX];
    if (snprintf(sub, sizeof(sub), "%s/%s", dir_root, dir_name) <= 0) {
        return false;
    }

    orca_manifest_t man;
    char man_path[ORCA_APPMGR_PATH_MAX];
    /* Манифест ищем по типу корня: app.json в /apps, module.json в /modules. */
    snprintf(man_path, sizeof(man_path), "%s/%s", sub,
             orca_manifest_filename(root_type));
    orca_manifest_load(man_path, &man);

    /* Кандидаты на имя образа, в порядке предпочтения. */
    const char* candidates[3];
    uint32_t    n_cand = 0;
    char        by_dir[ORCA_MANIFEST_ENTRY_MAX + 8];

    if (man.present && man.entry[0] != '\0') {
        candidates[n_cand++] = man.entry;
    }
    if (snprintf(by_dir, sizeof(by_dir), "%s.orca", dir_name) > 0) {
        candidates[n_cand++] = by_dir;
    }
    candidates[n_cand++] = (root_type == ORCA_MOD_TYPE_APP) ? "app.orca"
                                                            : "module.orca";

    for (uint32_t i = 0; i < n_cand; i++) {
        char img[ORCA_APPMGR_PATH_MAX];
        if (snprintf(img, sizeof(img), "%s/%s", sub, candidates[i]) <= 0) {
            continue;
        }
        FILINFO fno;
        if (f_stat(img, &fno) != FR_OK || (fno.fattrib & AM_DIR)) {
            continue;
        }

        orca_catalog_entry_t* e = &s_catalog[s_catalog_n];
        memset(e, 0, sizeof(*e));
        strncpy(e->path, img, sizeof(e->path) - 1);
        e->manifest = man;
        e->size     = (uint32_t)fno.fsize;
        e->type     = (man.present && man.type != ORCA_MOD_TYPE_UNKNOWN)
                          ? man.type : root_type;
        s_catalog_n++;
        return true;
    }
    return false;
}

/* Одиночный образ dir_root/<file>.orca без своего каталога. */
static bool catalog_add_file(const char* dir_root, const FILINFO* fno,
                             orca_module_type_t root_type)
{
    if (s_catalog_n >= ORCA_APPMGR_CATALOG_MAX) {
        return false;
    }
    uint32_t len = (uint32_t)strlen(fno->fname);
    if (len <= 5u || strcmp(&fno->fname[len - 5], ".orca") != 0) {
        return false;
    }

    orca_catalog_entry_t* e = &s_catalog[s_catalog_n];
    memset(e, 0, sizeof(*e));
    if (snprintf(e->path, sizeof(e->path), "%s/%s", dir_root, fno->fname) <= 0) {
        return false;
    }
    orca_manifest_default(&e->manifest);
    e->size = (uint32_t)fno->fsize;
    e->type = root_type;
    s_catalog_n++;
    return true;
}

static void scan_root(const char* dir_root, orca_module_type_t root_type)
{
    DIR dir;
    if (f_opendir(&dir, dir_root) != FR_OK) {
        return;
    }
    FILINFO fno;
    while (s_catalog_n < ORCA_APPMGR_CATALOG_MAX) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') {
            break;
        }
        if (fno.fattrib & AM_DIR) {
            catalog_add_dir(dir_root, fno.fname, root_type);
        } else {
            catalog_add_file(dir_root, &fno, root_type);
        }
    }
    f_closedir(&dir);
}

uint32_t orca_appmgr_scan(void)
{
    s_catalog_n = 0;
    scan_root(ORCA_APPMGR_APPS_DIR,    ORCA_MOD_TYPE_APP);
    scan_root(ORCA_APPMGR_MODULES_DIR, ORCA_MOD_TYPE_MODULE);
    s_catalog_valid = true;
    return s_catalog_n;
}

uint32_t orca_appmgr_catalog(orca_catalog_entry_t* out, uint32_t max_count)
{
    if (out == NULL) {
        return 0;
    }
    if (!s_catalog_valid) {
        orca_appmgr_scan();
    }
    uint32_t n = (s_catalog_n < max_count) ? s_catalog_n : max_count;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_catalog[i];
    }
    return n;
}

bool orca_appmgr_find(const char* name, orca_catalog_entry_t* out)
{
    if (name == NULL) {
        return false;
    }
    if (!s_catalog_valid) {
        orca_appmgr_scan();
    }
    for (uint32_t i = 0; i < s_catalog_n; i++) {
        char disp[ORCA_MANIFEST_NAME_MAX];
        orca_appmgr_entry_name(&s_catalog[i], disp, sizeof(disp));
        if (strcmp(disp, name) == 0) {
            if (out != NULL) {
                *out = s_catalog[i];
            }
            return true;
        }
    }
    return false;
}

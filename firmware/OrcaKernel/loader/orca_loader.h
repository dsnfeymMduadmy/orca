#ifndef ORCA_LOADER_H
#define ORCA_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include "orca_api.h"

typedef enum {
    ORCA_LOAD_OK = 0,
    ORCA_LOAD_ERR_IO,
    ORCA_LOAD_ERR_BAD_ELF,
    ORCA_LOAD_ERR_UNSUPPORTED_RELOC,
    ORCA_LOAD_ERR_UNDEFINED_SYMBOL,
    ORCA_LOAD_ERR_NO_MEMORY,
    ORCA_LOAD_ERR_NO_ENTRY,
    ORCA_LOAD_ERR_API_VERSION,
    ORCA_LOAD_ERR_BUSY          /* загрузчик занят другой задачей */
} orca_load_status_t;

typedef struct {
    void*             image_base;   /* базовый адрес в арене SDRAM */
    uint32_t          image_size;
    orca_app_main_fn  main_fn;
    orca_app_stop_fn  stop_fn;      /* может быть NULL */
    orca_cmd_main_fn  cmd_fn;       /* NULL, если это не команда shell'а */
    char              name[32];
} orca_loaded_module_t;

/*
 * Создаёт мьютекс загрузчика. Вызывается один раз при старте (до
 * vTaskStartScheduler) — orca_appmgr_init() делает это сам.
 */
void orca_loader_init(void);

/*
 * Загружает relocatable ELF32 (.orca файл) по пути VFS `path` в арену SDRAM,
 * резолвит внешние символы через `api`, применяет релокации и находит
 * точки входа orca_app_main/orca_app_stop/orca_cmd_main.
 *
 * Модуль обязан экспортировать хотя бы одну из точек входа: orca_app_main
 * (долгоживущий модуль со своей задачей) или orca_cmd_main (команда shell'а,
 * выполняется синхронно). Возвращает ORCA_LOAD_OK и заполняет `out`, либо
 * код ошибки.
 *
 * Разбор сериализован: одновременная загрузка из двух задач вернёт
 * ORCA_LOAD_ERR_BUSY, а не покорёженный образ.
 */
orca_load_status_t orca_loader_load(const char* path,
                                     const orca_api_t* api,
                                     orca_loaded_module_t* out);

/* Освобождает память арены, занятую модулем (вызывать после остановки таска). */
void orca_loader_unload(orca_loaded_module_t* mod);

/*
 * Синхронизация кешей после записи исполняемого кода в SDRAM.
 * Реализация зависит от ядра (orca_loader_cache.c для Cortex-M7).
 */
void orca_loader_cache_sync(void* addr, uint32_t size);

#endif /* ORCA_LOADER_H */

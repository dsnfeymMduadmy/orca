#ifndef ORCA_FILEIO_H
#define ORCA_FILEIO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Тонкая абстракция поверх FatFs (или другого backend), используемая
 * внутренними компонентами ядра (loader, appmgr). Отдельно от orca_api_t,
 * который видят только загруженные модули/приложения.
 */

typedef struct orca_file orca_file_t; /* opaque */

/*
 * Режим открытия. Отдельный enum, а не orca_file_mode_t из orca_api.h:
 * fs-слой не должен зависеть от ABI модулей.
 */
typedef enum {
    ORCA_FIO_READ = 0,
    ORCA_FIO_WRITE,   /* создать/обрезать */
    ORCA_FIO_APPEND   /* создать при отсутствии, писать в конец */
} orca_fio_mode_t;

orca_file_t* orca_fileio_open(const char* path, bool write);
orca_file_t* orca_fileio_open_ex(const char* path, orca_fio_mode_t mode);
int32_t      orca_fileio_read(orca_file_t* f, void* buf, uint32_t len);
int32_t      orca_fileio_write(orca_file_t* f, const void* buf, uint32_t len);
bool         orca_fileio_seek(orca_file_t* f, uint32_t offset);
uint32_t     orca_fileio_size(orca_file_t* f);
void         orca_fileio_close(orca_file_t* f);
bool         orca_fileio_mkdir(const char* path);

#endif /* ORCA_FILEIO_H */

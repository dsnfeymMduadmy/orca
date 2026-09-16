#include "orca_fileio.h"
#include "ff.h" /* FatFs — из firmware/Middlewares/FatFs, добавляется отдельно */
#include <stdlib.h>

struct orca_file {
    FIL fil;
};

orca_file_t* orca_fileio_open_ex(const char* path, orca_fio_mode_t mode)
{
    if (path == NULL) {
        return NULL;
    }

    orca_file_t* f = (orca_file_t*)malloc(sizeof(orca_file_t));
    if (f == NULL) {
        return NULL;
    }

    BYTE flags;
    switch (mode) {
        case ORCA_FIO_WRITE:  flags = FA_WRITE | FA_CREATE_ALWAYS;      break;
        case ORCA_FIO_APPEND: flags = FA_WRITE | FA_OPEN_ALWAYS;        break;
        default:              flags = FA_READ;                          break;
    }

    if (f_open(&f->fil, path, flags) != FR_OK) {
        free(f);
        return NULL;
    }

    /* FA_OPEN_ALWAYS ставит курсор в начало — для append его надо доехать до конца. */
    if (mode == ORCA_FIO_APPEND && f_lseek(&f->fil, f_size(&f->fil)) != FR_OK) {
        f_close(&f->fil);
        free(f);
        return NULL;
    }
    return f;
}

orca_file_t* orca_fileio_open(const char* path, bool write)
{
    return orca_fileio_open_ex(path, write ? ORCA_FIO_WRITE : ORCA_FIO_READ);
}

int32_t orca_fileio_read(orca_file_t* f, void* buf, uint32_t len)
{
    if (f == NULL) {
        return -1;
    }
    UINT read = 0;
    if (f_read(&f->fil, buf, len, &read) != FR_OK) {
        return -1;
    }
    return (int32_t)read;
}

int32_t orca_fileio_write(orca_file_t* f, const void* buf, uint32_t len)
{
    if (f == NULL) {
        return -1;
    }
    UINT written = 0;
    if (f_write(&f->fil, buf, len, &written) != FR_OK) {
        return -1;
    }
    return (int32_t)written;
}

bool orca_fileio_seek(orca_file_t* f, uint32_t offset)
{
    if (f == NULL) {
        return false;
    }
    return f_lseek(&f->fil, offset) == FR_OK;
}

uint32_t orca_fileio_size(orca_file_t* f)
{
    if (f == NULL) {
        return 0;
    }
    return (uint32_t)f_size(&f->fil);
}

void orca_fileio_close(orca_file_t* f)
{
    if (f == NULL) {
        return;
    }
    f_close(&f->fil);
    free(f);
}

bool orca_fileio_mkdir(const char* path)
{
    if (path == NULL) {
        return false;
    }
    return f_mkdir(path) == FR_OK;
}

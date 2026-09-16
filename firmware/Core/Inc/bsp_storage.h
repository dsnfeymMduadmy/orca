#ifndef BSP_STORAGE_H
#define BSP_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Блочные устройства платы: microSD (том FatFs "0:") и QSPI-флешка 16 МБ
 * (том "1:").
 *
 * Блочный доступ вынесен в общий интерфейс, а не спрятан внутри diskio,
 * потому что у каждого носителя два независимых потребителя: ядро через
 * FatFs (Core/Src/bsp_diskio.c) и хост через USB MSC
 * (OrcaKernel/link/orca_usb_msc.h), а SCSI-командам файловая система ни к
 * чему — им нужны ровно «прочитай/запиши N блоков с LBA».
 *
 * Логический блок везде 512 байт: столько у SD-карты, столько ждёт SCSI
 * READ(10)/WRITE(10), и с таким же FatFs собран (FF_MIN_SS = FF_MAX_SS).
 */
#define BSP_BLOCK_SIZE   512u

/* ------------------------------------------------------------------ */
/* microSD — том "0:"                                                  */
/* ------------------------------------------------------------------ */

bool     bsp_sdmmc_init(void);
bool     bsp_sdmmc_ready(void);
uint32_t bsp_sdmmc_block_count(void);
bool     bsp_sdmmc_block_read(uint8_t* buf, uint32_t lba, uint32_t count);
bool     bsp_sdmmc_block_write(const uint8_t* buf, uint32_t lba, uint32_t count);
bool     bsp_sdmmc_sync(void);

/*
 * Монтирование разъёмное: пока носитель отдан хосту по USB, ядро обязано
 * его отпустить. Хост кеширует FAT у себя и не ждёт, что содержимое
 * изменится под ним, так что два владельца одной ФС — это битая ФС.
 */
bool     bsp_sdmmc_mount(void);
bool     bsp_sdmmc_unmount(void);
bool     bsp_sdmmc_is_mounted(void);

/* ------------------------------------------------------------------ */
/* QSPI 16 МБ — том "1:"                                               */
/* ------------------------------------------------------------------ */

bool     bsp_qspi_init(void);
bool     bsp_qspi_ready(void);

/*
 * Причина отказа и ответ чипа на JEDEC ID (0x9F): «QSPI init failed»
 * одинаково выглядит и когда чипа нет, и когда перепутана распиновка.
 */
const char*    bsp_qspi_error(void);
const uint8_t* bsp_qspi_jedec_id(void);

uint32_t bsp_qspi_block_count(void);
/* Стираемая единица в блоках: FatFs выравнивает по ней данные тома. */
uint32_t bsp_qspi_erase_blocks(void);
bool     bsp_qspi_block_read(uint8_t* buf, uint32_t lba, uint32_t count);
bool     bsp_qspi_block_write(const uint8_t* buf, uint32_t lba, uint32_t count);
bool     bsp_qspi_sync(void);

bool     bsp_qspi_mount(void);
bool     bsp_qspi_unmount(void);
bool     bsp_qspi_is_mounted(void);

#endif /* BSP_STORAGE_H */

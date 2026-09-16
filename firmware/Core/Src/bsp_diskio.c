#include "bsp_storage.h"
#include "board.h"
#include "ff.h"
#include "diskio.h"

/*
 * Диспетчер diskio для FatFs. Носителей два, а точка входа у FatFs одна,
 * поэтому disk_* живут здесь, а не в драйверах: иначе первый же второй том
 * означал бы два определения disk_read в проекте.
 *
 *   pdrv 0 -> "0:" microSD  (Core/Src/bsp_sdmmc.c)
 *   pdrv 1 -> "1:" QSPI     (Core/Src/bsp_qspi.c)
 *
 * Нумерация обязана совпадать с FF_VOLUME_STRS в ffconf.h.
 */

#define DEV_SD      0
#define DEV_QSPI    1

DSTATUS disk_status(BYTE pdrv)
{
    switch (pdrv) {
        case DEV_SD:   return bsp_sdmmc_ready() ? 0 : STA_NOINIT;
        case DEV_QSPI: return bsp_qspi_ready()  ? 0 : STA_NOINIT;
        default:       return STA_NOINIT;
    }
}

/*
 * Железо поднимают bsp_*_init на старте системы, до всякого f_mount: SDMMC
 * и QUADSPI нужны не только файловой системе (USB MSC работает с ними в
 * обход FatFs), и порядок инициализации не должен зависеть от того, кто
 * первым дёрнул монтирование.
 */
DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
{
    bool ok;

    switch (pdrv) {
        case DEV_SD:
            ok = bsp_sdmmc_block_read(buff, (uint32_t)sector, count);
            break;
        case DEV_QSPI:
            ok = bsp_qspi_block_read(buff, (uint32_t)sector, count);
            break;
        default:
            return RES_PARERR;
    }
    return ok ? RES_OK : RES_ERROR;
}

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
{
    bool ok;

    switch (pdrv) {
        case DEV_SD:
            ok = bsp_sdmmc_block_write(buff, (uint32_t)sector, count);
            break;
        case DEV_QSPI:
            ok = bsp_qspi_block_write(buff, (uint32_t)sector, count);
            break;
        default:
            return RES_PARERR;
    }
    return ok ? RES_OK : RES_ERROR;
}
#endif

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff)
{
    if (pdrv != DEV_SD && pdrv != DEV_QSPI) {
        return RES_PARERR;
    }

    switch (cmd) {
        case CTRL_SYNC:
            if (pdrv == DEV_SD) {
                return bsp_sdmmc_sync() ? RES_OK : RES_ERROR;
            }
            return bsp_qspi_sync() ? RES_OK : RES_ERROR;

        case GET_SECTOR_COUNT:
            *(LBA_t*)buff = (pdrv == DEV_SD) ? bsp_sdmmc_block_count()
                                             : bsp_qspi_block_count();
            return RES_OK;

        case GET_SECTOR_SIZE:
            *(WORD*)buff = BSP_BLOCK_SIZE;
            return RES_OK;

        /*
         * Размер стираемого блока в секторах. Для карты он неизвестен и не
         * важен (контроллер карты сам управляет стиранием), а для NOR это
         * настоящие 4 КБ: по ним f_mkfs выравнивает область данных, чтобы
         * кластер не лежал на границе двух секторов стирания.
         */
        case GET_BLOCK_SIZE:
            *(DWORD*)buff = (pdrv == DEV_SD) ? 1u : bsp_qspi_erase_blocks();
            return RES_OK;

        default:
            return RES_PARERR;
    }
}

/*
 * Время для меток файлов. Локальное, а не UTC: FAT хранит время без пояса,
 * и пользователь сравнивает его с часами на экране, а не с гринвичем.
 * Часы не выставлены — отдаём фиксированную дату: с нулевым годом FAT
 * некоторые хосты показывают запись как повреждённую.
 */
DWORD get_fattime(void)
{
    if (board_rtc_time_valid()) {
        orca_datetime_t dt;

        board_rtc_split(board_rtc_unix() + (uint64_t)((int64_t)board_rtc_tz_offset() * 60),
                        &dt);

        if (dt.year >= 1980) {
            return ((DWORD)(dt.year - 1980) << 25) |
                   ((DWORD)dt.month  << 21) |
                   ((DWORD)dt.day    << 16) |
                   ((DWORD)dt.hour   << 11) |
                   ((DWORD)dt.minute << 5)  |
                   ((DWORD)(dt.second / 2));
        }
    }

    return ((DWORD)(2025 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}

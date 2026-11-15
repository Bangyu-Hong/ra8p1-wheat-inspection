#include "diskio.h"

#include <stdint.h>

#include "sd_media.h"

#define FATFS_SD_DRIVE  (0U)

static DSTATUS disk_status_from_media(void);

DSTATUS disk_initialize (BYTE pdrv)
{
    app_status_t status;

    if (FATFS_SD_DRIVE != pdrv)
    {
        return STA_NOINIT;
    }

    status = sd_media_open();
    if (APP_STATUS_OK == status)
    {
        status = sd_media_initialize();
    }
    return (APP_STATUS_OK == status) ? disk_status_from_media() : STA_NOINIT;
}

DSTATUS disk_status (BYTE pdrv)
{
    if (FATFS_SD_DRIVE != pdrv)
    {
        return (DSTATUS) (STA_NOINIT | STA_NODISK);
    }
    sd_media_process();
    return disk_status_from_media();
}

DRESULT disk_read (BYTE pdrv, BYTE * buff, LBA_t sector, UINT count)
{
    app_status_t status;

    if ((FATFS_SD_DRIVE != pdrv) || (NULL == buff) || (0U == count) ||
        (sector > UINT32_MAX) || (count > SD_MEDIA_MAX_SECTORS_PER_CALL))
    {
        return RES_PARERR;
    }

    status = sd_media_read(buff, (uint32_t) sector, (uint32_t) count);
    if (APP_STATUS_OK == status)
    {
        return RES_OK;
    }
    return ((APP_STATUS_NOT_INITIALIZED == status) ||
            (APP_STATUS_UNAVAILABLE == status)) ? RES_NOTRDY : RES_ERROR;
}

DRESULT disk_write (BYTE pdrv, BYTE const * buff, LBA_t sector, UINT count)
{
    app_status_t status;
    sd_media_diagnostics_t diagnostics;

    if ((FATFS_SD_DRIVE != pdrv) || (NULL == buff) || (0U == count) ||
        (sector > UINT32_MAX) || (count > SD_MEDIA_MAX_SECTORS_PER_CALL))
    {
        return RES_PARERR;
    }

    sd_media_get_diagnostics(&diagnostics);
    if (diagnostics.write_protected)
    {
        return RES_WRPRT;
    }
    status = sd_media_write(buff, (uint32_t) sector, (uint32_t) count);
    if (APP_STATUS_OK == status)
    {
        return RES_OK;
    }
    return ((APP_STATUS_NOT_INITIALIZED == status) ||
            (APP_STATUS_UNAVAILABLE == status)) ? RES_NOTRDY : RES_ERROR;
}

DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void * buff)
{
    sd_media_diagnostics_t diagnostics;

    if (FATFS_SD_DRIVE != pdrv)
    {
        return RES_PARERR;
    }

    sd_media_get_diagnostics(&diagnostics);
    switch (cmd)
    {
        case CTRL_SYNC:
            return (APP_STATUS_OK == sd_media_sync()) ? RES_OK : RES_NOTRDY;

        case GET_SECTOR_COUNT:
            if (NULL == buff)
            {
                return RES_PARERR;
            }
            *((LBA_t *) buff) = (LBA_t) diagnostics.sector_count;
            return diagnostics.initialized ? RES_OK : RES_NOTRDY;

        case GET_SECTOR_SIZE:
            if (NULL == buff)
            {
                return RES_PARERR;
            }
            *((WORD *) buff) = (WORD) SD_MEDIA_SECTOR_BYTES;
            return RES_OK;

        case GET_BLOCK_SIZE:
            if (NULL == buff)
            {
                return RES_PARERR;
            }
            *((DWORD *) buff) = 1U;
            return RES_OK;

        default:
            return RES_PARERR;
    }
}

static DSTATUS disk_status_from_media (void)
{
    DSTATUS status = 0U;
    sd_media_diagnostics_t diagnostics;

    sd_media_get_diagnostics(&diagnostics);
    if (!diagnostics.initialized)
    {
        status |= STA_NOINIT;
    }
    if (!diagnostics.media_inserted)
    {
        status |= STA_NODISK;
    }
    if (diagnostics.write_protected)
    {
        status |= STA_PROTECT;
    }
    return status;
}

#include "sd_media.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(SD_MEDIA_HOST_TEST)
 #include "hal_data.h"
 #include "platform_time.h"
#endif

static volatile uint32_t g_sd_media_events;
static sd_media_diagnostics_t g_sd_media_diagnostics;

static uint32_t sd_media_events_load(void);
static void sd_media_events_set(uint32_t events);
static void sd_media_events_clear(uint32_t events);

#if !defined(SD_MEDIA_HOST_TEST)
static uint8_t g_sd_media_bounce[SD_MEDIA_SECTOR_BYTES]
    __attribute__((aligned(SD_MEDIA_CACHE_LINE_BYTES)));

static app_status_t sd_media_transfer(bool write,
                                      uint8_t * p_buffer,
                                      uint32_t sector,
                                      uint32_t count);
static app_status_t sd_media_transfer_direct(bool write,
                                             uint8_t * p_buffer,
                                             uint32_t sector,
                                             uint32_t count);
static app_status_t sd_media_wait_for_operation(uint64_t start_us);
static app_status_t sd_media_refresh_status(void);
static app_status_t sd_media_from_fsp(fsp_err_t error);
#endif

bool sd_media_buffer_is_aligned (void const * p_buffer)
{
    return (NULL != p_buffer) &&
           (0U == ((uintptr_t) p_buffer & (SD_MEDIA_CACHE_LINE_BYTES - 1U)));
}

app_status_t sd_media_evaluate_operation (uint32_t events, uint64_t elapsed_us)
{
    if (0U != (events & SD_MEDIA_EVENT_REMOVED))
    {
        return APP_STATUS_UNAVAILABLE;
    }
    if (0U != (events & SD_MEDIA_EVENT_ERROR))
    {
        return APP_STATUS_IO;
    }
    if (0U != (events & SD_MEDIA_EVENT_COMPLETE))
    {
        return APP_STATUS_OK;
    }
    return (elapsed_us >= SD_MEDIA_TRANSFER_TIMEOUT_US) ?
           APP_STATUS_TIMEOUT :
           APP_STATUS_BUSY;
}

#if !defined(SD_MEDIA_HOST_TEST)

app_status_t sd_media_open (void)
{
    fsp_err_t error;

    if (g_sd_media_diagnostics.opened)
    {
        return APP_STATUS_OK;
    }

    g_sd_media_events = SD_MEDIA_EVENT_NONE;
    g_sd_media_diagnostics = (sd_media_diagnostics_t) {0};
    error = g_sd_block_media.p_api->open(g_sd_block_media.p_ctrl,
                                         g_sd_block_media.p_cfg);
    g_sd_media_diagnostics.last_fsp_error = (int32_t) error;
    if (FSP_SUCCESS != error)
    {
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_DEGRADED;
        return sd_media_from_fsp(error);
    }

    g_sd_media_diagnostics.opened = true;
    g_sd_media_diagnostics.state = SD_MEDIA_STATE_NO_CARD;
    return sd_media_refresh_status();
}

app_status_t sd_media_initialize (void)
{
    rm_block_media_info_t info;
    fsp_err_t error;
    app_status_t status;

    if (!g_sd_media_diagnostics.opened)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    status = sd_media_refresh_status();
    if ((APP_STATUS_OK != status) || !g_sd_media_diagnostics.media_inserted)
    {
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_NO_CARD;
        return APP_STATUS_UNAVAILABLE;
    }

    g_sd_media_diagnostics.state = SD_MEDIA_STATE_INITIALIZING;
    error = g_sd_block_media.p_api->mediaInit(g_sd_block_media.p_ctrl);
    g_sd_media_diagnostics.last_fsp_error = (int32_t) error;
    if (FSP_SUCCESS != error)
    {
        g_sd_media_diagnostics.initialized = false;
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_DEGRADED;
        return sd_media_from_fsp(error);
    }

    error = g_sd_block_media.p_api->infoGet(g_sd_block_media.p_ctrl, &info);
    g_sd_media_diagnostics.last_fsp_error = (int32_t) error;
    if (FSP_SUCCESS != error)
    {
        g_sd_media_diagnostics.initialized = false;
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_DEGRADED;
        return sd_media_from_fsp(error);
    }
    if (SD_MEDIA_SECTOR_BYTES != info.sector_size_bytes)
    {
        g_sd_media_diagnostics.initialized = false;
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_DEGRADED;
        return APP_STATUS_DATA;
    }

    g_sd_media_diagnostics.sector_size_bytes = info.sector_size_bytes;
    g_sd_media_diagnostics.sector_count = info.num_sectors;
    g_sd_media_diagnostics.write_protected = info.write_protected;
    g_sd_media_diagnostics.initialized = true;
    g_sd_media_diagnostics.state = SD_MEDIA_STATE_READY;
    return APP_STATUS_OK;
}

app_status_t sd_media_read (uint8_t * p_destination,
                            uint32_t sector,
                            uint32_t count)
{
    return sd_media_transfer(false, p_destination, sector, count);
}

app_status_t sd_media_write (uint8_t const * p_source,
                             uint32_t sector,
                             uint32_t count)
{
    return sd_media_transfer(true, (uint8_t *) p_source, sector, count);
}

app_status_t sd_media_sync (void)
{
    rm_block_media_status_t media_status;
    fsp_err_t error;

    if (!g_sd_media_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    error = g_sd_block_media.p_api->statusGet(g_sd_block_media.p_ctrl,
                                              &media_status);
    g_sd_media_diagnostics.last_fsp_error = (int32_t) error;
    if (FSP_SUCCESS != error)
    {
        return sd_media_from_fsp(error);
    }
    if (!media_status.media_inserted)
    {
        g_sd_media_diagnostics.media_inserted = false;
        g_sd_media_diagnostics.initialized = false;
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_NO_CARD;
        return APP_STATUS_UNAVAILABLE;
    }
    return media_status.busy ? APP_STATUS_BUSY : APP_STATUS_OK;
}

app_status_t sd_media_close (void)
{
    fsp_err_t error;

    if (!g_sd_media_diagnostics.opened)
    {
        return APP_STATUS_OK;
    }

    error = g_sd_block_media.p_api->close(g_sd_block_media.p_ctrl);
    g_sd_media_diagnostics.last_fsp_error = (int32_t) error;
    g_sd_media_diagnostics.opened = false;
    g_sd_media_diagnostics.initialized = false;
    g_sd_media_diagnostics.state = SD_MEDIA_STATE_CLOSED;
    return sd_media_from_fsp(error);
}

void sd_media_process (void)
{
    uint32_t const events = sd_media_events_load();

    if (0U != (events & SD_MEDIA_EVENT_REMOVED))
    {
        sd_media_events_clear(SD_MEDIA_EVENT_REMOVED);
        g_sd_media_diagnostics.removals++;
        g_sd_media_diagnostics.media_inserted = false;
        g_sd_media_diagnostics.initialized = false;
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_NO_CARD;
    }
    if (0U != (events & SD_MEDIA_EVENT_INSERTED))
    {
        sd_media_events_clear(SD_MEDIA_EVENT_INSERTED);
        g_sd_media_diagnostics.insertions++;
        g_sd_media_diagnostics.media_inserted = true;
        if (SD_MEDIA_STATE_BUSY != g_sd_media_diagnostics.state)
        {
            g_sd_media_diagnostics.state = SD_MEDIA_STATE_INITIALIZING;
        }
    }
}

void sd_media_get_diagnostics (sd_media_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        *p_diagnostics = g_sd_media_diagnostics;
    }
}

void sd_media_callback (rm_block_media_callback_args_t * p_args)
{
    rm_block_media_event_t const event = (NULL == p_args) ? 0U : p_args->event;

    if (0U != (event & RM_BLOCK_MEDIA_EVENT_OPERATION_COMPLETE))
    {
        sd_media_events_set(SD_MEDIA_EVENT_COMPLETE);
    }
    if (0U != (event & RM_BLOCK_MEDIA_EVENT_ERROR))
    {
        sd_media_events_set(SD_MEDIA_EVENT_ERROR);
    }
    if (0U != (event & RM_BLOCK_MEDIA_EVENT_MEDIA_INSERTED))
    {
        sd_media_events_set(SD_MEDIA_EVENT_INSERTED);
    }
    if (0U != (event & RM_BLOCK_MEDIA_EVENT_MEDIA_REMOVED))
    {
        sd_media_events_set(SD_MEDIA_EVENT_REMOVED);
    }
}

static app_status_t sd_media_transfer (bool write,
                                       uint8_t * p_buffer,
                                       uint32_t sector,
                                       uint32_t count)
{
    app_status_t status;

    if ((NULL == p_buffer) || (0U == count) ||
        (count > SD_MEDIA_MAX_SECTORS_PER_CALL) ||
        ((sector + count) < sector))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (!g_sd_media_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if (write && g_sd_media_diagnostics.write_protected)
    {
        return APP_STATUS_IO;
    }

    if (sd_media_buffer_is_aligned(p_buffer))
    {
        g_sd_media_diagnostics.direct_transfers++;
        return sd_media_transfer_direct(write, p_buffer, sector, count);
    }

    g_sd_media_diagnostics.bounce_transfers += count;
    for (uint32_t index = 0U; index < count; index++)
    {
        uint8_t * p_sector_buffer = p_buffer + (index * SD_MEDIA_SECTOR_BYTES);

        if (write)
        {
            memcpy(g_sd_media_bounce, p_sector_buffer, SD_MEDIA_SECTOR_BYTES);
        }
        status = sd_media_transfer_direct(write,
                                          g_sd_media_bounce,
                                          sector + index,
                                          1U);
        if (APP_STATUS_OK != status)
        {
            return status;
        }
        if (!write)
        {
            memcpy(p_sector_buffer, g_sd_media_bounce, SD_MEDIA_SECTOR_BYTES);
        }
    }

    return APP_STATUS_OK;
}

static app_status_t sd_media_transfer_direct (bool write,
                                              uint8_t * p_buffer,
                                              uint32_t sector,
                                              uint32_t count)
{
    uint64_t start_us = 0U;
    fsp_err_t error;
    app_status_t status;

    status = platform_time_now(&start_us);
    if ((APP_STATUS_OK != status) && (APP_STATUS_TIMEOUT != status))
    {
        return status;
    }

    sd_media_events_clear(SD_MEDIA_EVENT_COMPLETE |
                          SD_MEDIA_EVENT_ERROR |
                          SD_MEDIA_EVENT_REMOVED);
    g_sd_media_diagnostics.state = SD_MEDIA_STATE_BUSY;
    if (write)
    {
        error = g_sd_block_media.p_api->write(g_sd_block_media.p_ctrl,
                                              p_buffer,
                                              sector,
                                              count);
    }
    else
    {
        error = g_sd_block_media.p_api->read(g_sd_block_media.p_ctrl,
                                             p_buffer,
                                             sector,
                                             count);
    }
    g_sd_media_diagnostics.last_fsp_error = (int32_t) error;
    if (FSP_SUCCESS != error)
    {
        g_sd_media_diagnostics.operation_errors++;
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_DEGRADED;
        return sd_media_from_fsp(error);
    }

    return sd_media_wait_for_operation(start_us);
}

static app_status_t sd_media_wait_for_operation (uint64_t start_us)
{
    uint64_t now_us = start_us;

    while ((now_us - start_us) < SD_MEDIA_TRANSFER_TIMEOUT_US)
    {
        uint32_t const events = sd_media_events_load();
        app_status_t const operation_status =
            sd_media_evaluate_operation(events, now_us - start_us);

        if (APP_STATUS_UNAVAILABLE == operation_status)
        {
            g_sd_media_diagnostics.removals++;
            g_sd_media_diagnostics.media_inserted = false;
            g_sd_media_diagnostics.initialized = false;
            g_sd_media_diagnostics.state = SD_MEDIA_STATE_NO_CARD;
            return APP_STATUS_UNAVAILABLE;
        }
        if (APP_STATUS_IO == operation_status)
        {
            g_sd_media_diagnostics.operation_errors++;
            g_sd_media_diagnostics.state = SD_MEDIA_STATE_DEGRADED;
            return APP_STATUS_IO;
        }
        if (APP_STATUS_OK == operation_status)
        {
            g_sd_media_diagnostics.operations_completed++;
            g_sd_media_diagnostics.state = SD_MEDIA_STATE_READY;
            return APP_STATUS_OK;
        }
        app_status_t const time_status = platform_time_now(&now_us);
        if ((APP_STATUS_OK != time_status) &&
            (APP_STATUS_TIMEOUT != time_status))
        {
            break;
        }
    }

    g_sd_media_diagnostics.operation_timeouts++;
    g_sd_media_diagnostics.state = SD_MEDIA_STATE_DEGRADED;
    return APP_STATUS_TIMEOUT;
}

static app_status_t sd_media_refresh_status (void)
{
    rm_block_media_status_t media_status;
    fsp_err_t error = g_sd_block_media.p_api->statusGet(g_sd_block_media.p_ctrl,
                                                        &media_status);

    g_sd_media_diagnostics.last_fsp_error = (int32_t) error;
    if (FSP_SUCCESS != error)
    {
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_DEGRADED;
        return sd_media_from_fsp(error);
    }

    g_sd_media_diagnostics.media_inserted = media_status.media_inserted;
    g_sd_media_diagnostics.initialized = media_status.initialized;
    if (!media_status.media_inserted)
    {
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_NO_CARD;
    }
    return APP_STATUS_OK;
}

static app_status_t sd_media_from_fsp (fsp_err_t error)
{
    if (FSP_SUCCESS == error)
    {
        return APP_STATUS_OK;
    }
    if (FSP_ERR_TIMEOUT == error)
    {
        return APP_STATUS_TIMEOUT;
    }
    if ((FSP_ERR_CARD_NOT_INSERTED == error) ||
        (FSP_ERR_CARD_NOT_INITIALIZED == error))
    {
        return APP_STATUS_UNAVAILABLE;
    }
    if (FSP_ERR_DEVICE_BUSY == error)
    {
        return APP_STATUS_BUSY;
    }
    return APP_STATUS_IO;
}

#else

app_status_t sd_media_open (void) { return APP_STATUS_UNAVAILABLE; }
app_status_t sd_media_initialize (void) { return APP_STATUS_UNAVAILABLE; }
app_status_t sd_media_read (uint8_t * p_destination, uint32_t sector, uint32_t count)
{
    (void) p_destination;
    (void) sector;
    (void) count;
    return APP_STATUS_UNAVAILABLE;
}
app_status_t sd_media_write (uint8_t const * p_source, uint32_t sector, uint32_t count)
{
    (void) p_source;
    (void) sector;
    (void) count;
    return APP_STATUS_UNAVAILABLE;
}
app_status_t sd_media_sync (void) { return APP_STATUS_UNAVAILABLE; }
app_status_t sd_media_close (void) { return APP_STATUS_OK; }
void sd_media_process (void)
{
    uint32_t const events = sd_media_events_load();

    if (0U != (events & SD_MEDIA_EVENT_REMOVED))
    {
        sd_media_events_clear(SD_MEDIA_EVENT_REMOVED);
        g_sd_media_diagnostics.removals++;
        g_sd_media_diagnostics.media_inserted = false;
        g_sd_media_diagnostics.initialized = false;
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_NO_CARD;
    }
    if (0U != (events & SD_MEDIA_EVENT_INSERTED))
    {
        sd_media_events_clear(SD_MEDIA_EVENT_INSERTED);
        g_sd_media_diagnostics.insertions++;
        g_sd_media_diagnostics.media_inserted = true;
        g_sd_media_diagnostics.state = SD_MEDIA_STATE_INITIALIZING;
    }
}
void sd_media_get_diagnostics (sd_media_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        *p_diagnostics = g_sd_media_diagnostics;
    }
}

void sd_media_host_inject_events (uint32_t events)
{
    sd_media_events_set(events);
}

#endif

static uint32_t sd_media_events_load (void)
{
    return __atomic_load_n(&g_sd_media_events, __ATOMIC_RELAXED);
}

static void sd_media_events_set (uint32_t events)
{
    (void) __atomic_fetch_or(&g_sd_media_events, events, __ATOMIC_RELAXED);
}

static void sd_media_events_clear (uint32_t events)
{
    (void) __atomic_fetch_and(&g_sd_media_events, ~events, __ATOMIC_RELAXED);
}

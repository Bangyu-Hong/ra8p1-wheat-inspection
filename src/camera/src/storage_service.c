#include "storage_service.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "fatfs/ff.h"
#include "sd_media.h"
#include "storage_journal.h"

#define STORAGE_BINARY_PATH          "0:/FIELD.BIN"
#define STORAGE_CSV_PATH             "0:/FIELD.CSV"
#define STORAGE_RETRY_US             (5000000ULL)
#define STORAGE_MAX_FAILURES         (3U)
#define STORAGE_CSV_ROW_BYTES        (768U)
#define STORAGE_CSV_HEADER \
    "schema,sequence,monotonic_us,temperature_milli_c,humidity_milli_percent," \
    "environment_valid,light_raw,light_average,light_tenths_percent,light_valid," \
    "wheat_count,inference_us,camera_state,ai_state,sd_state,network_state," \
    "fault_flags,field_row,field_column,mission_wheat_count,field_wheat_count,counted_frame_count\r\n"

typedef struct st_storage_context
{
    storage_journal_queue_t queue;
    storage_service_diagnostics_t diagnostics;
    FATFS filesystem;
    FIL binary_file;
    FIL csv_file;
    uint8_t encoded[STORAGE_JOURNAL_HEADER_BYTES + STORAGE_JOURNAL_MAX_PAYLOAD_BYTES]
        __attribute__((aligned(SD_MEDIA_CACHE_LINE_BYTES)));
    char csv_row[STORAGE_CSV_ROW_BYTES];
    char enqueue_payload[STORAGE_CSV_ROW_BYTES];
    uint32_t encoded_bytes;
    uint32_t csv_row_bytes;
    uint32_t recover_last_good_offset;
    uint32_t recover_payload_bytes;
    uint32_t recover_last_sequence;
    uint64_t retry_deadline_us;
    uint32_t media_insertions_seen;
    uint8_t consecutive_failures;
    bool recovered_record_valid;
    bool pending_binary;
    bool binary_open;
    bool csv_open;
} storage_context_t;

static storage_context_t g_storage;

static app_status_t storage_step(uint64_t now_us);
static app_status_t storage_fail(app_status_t status,
                                 uint64_t now_us,
                                 bool filesystem_failure);
static app_status_t storage_format_telemetry(
    app_telemetry_snapshot_t const * p_snapshot,
    char * p_output,
    uint32_t output_capacity,
    uint32_t * p_output_bytes);
static app_status_t storage_from_fresult(FRESULT result);
static void storage_refresh_queue_metrics(void);

app_status_t storage_service_init (uint64_t now_us)
{
    if (g_storage.diagnostics.initialized)
    {
        return APP_STATUS_OK;
    }

    g_storage = (storage_context_t) {0};
    storage_journal_init(&g_storage.queue);
    g_storage.diagnostics.initialized = true;
    g_storage.diagnostics.state = STORAGE_SERVICE_STATE_MEDIA_OPEN;
    g_storage.diagnostics.last_status = APP_STATUS_OK;
    g_storage.retry_deadline_us = now_us;
    return APP_STATUS_OK;
}

app_status_t storage_service_enqueue_telemetry (
    app_telemetry_snapshot_t const * p_snapshot)
{
    uint32_t payload_bytes = 0U;
    app_status_t status;

    if ((NULL == p_snapshot) || !g_storage.diagnostics.initialized)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    status = storage_format_telemetry(p_snapshot,
                                      g_storage.enqueue_payload,
                                      sizeof(g_storage.enqueue_payload),
                                      &payload_bytes);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    status = storage_journal_enqueue(&g_storage.queue,
                                     STORAGE_RECORD_TELEMETRY,
                                     p_snapshot->sequence,
                                     p_snapshot->monotonic_us,
                                     (uint8_t const *) g_storage.enqueue_payload,
                                     payload_bytes);
    storage_refresh_queue_metrics();
    return status;
}

app_status_t storage_service_process (uint64_t now_us, bool allow_filesystem_io)
{
    sd_media_diagnostics_t media;

    if (!g_storage.diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    sd_media_process();
    sd_media_get_diagnostics(&media);
    if (!media.media_inserted &&
        (STORAGE_SERVICE_STATE_MEDIA_OPEN != g_storage.diagnostics.state) &&
        (STORAGE_SERVICE_STATE_CLOSED != g_storage.diagnostics.state))
    {
        if (g_storage.diagnostics.mounted)
        {
            (void) f_mount(NULL, "0:", 0U);
        }
        g_storage.diagnostics.mounted = false;
        g_storage.diagnostics.writable = false;
        g_storage.binary_open = false;
        g_storage.csv_open = false;
        g_storage.diagnostics.state = STORAGE_SERVICE_STATE_WAIT_CARD;
        g_storage.diagnostics.last_status = APP_STATUS_UNAVAILABLE;
    }
    if (media.insertions != g_storage.media_insertions_seen)
    {
        g_storage.media_insertions_seen = media.insertions;
        if (media.media_inserted &&
            ((STORAGE_SERVICE_STATE_DEGRADED == g_storage.diagnostics.state) ||
             (STORAGE_SERVICE_STATE_BACKOFF == g_storage.diagnostics.state)))
        {
            g_storage.consecutive_failures = 0U;
            g_storage.retry_deadline_us = now_us;
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_MOUNT;
        }
    }

    if (!allow_filesystem_io)
    {
        return APP_STATUS_BUSY;
    }

    return storage_step(now_us);
}

app_status_t storage_service_close (void)
{
    FRESULT result = FR_OK;

    if (!g_storage.diagnostics.initialized)
    {
        return APP_STATUS_OK;
    }
    if (g_storage.csv_open)
    {
        result = f_close(&g_storage.csv_file);
        g_storage.csv_open = false;
    }
    if (g_storage.binary_open)
    {
        FRESULT const binary_result = f_close(&g_storage.binary_file);
        g_storage.binary_open = false;
        if (FR_OK == result)
        {
            result = binary_result;
        }
    }
    (void) f_mount(NULL, "0:", 0U);
    (void) sd_media_close();
    g_storage.diagnostics.mounted = false;
    g_storage.diagnostics.writable = false;
    g_storage.diagnostics.initialized = false;
    g_storage.diagnostics.state = STORAGE_SERVICE_STATE_CLOSED;
    return storage_from_fresult(result);
}

void storage_service_get_diagnostics (storage_service_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        storage_refresh_queue_metrics();
        *p_diagnostics = g_storage.diagnostics;
    }
}

uint8_t storage_service_telemetry_state (void)
{
    if (STORAGE_SERVICE_STATE_READY == g_storage.diagnostics.state)
    {
        return 2U;
    }
    if (STORAGE_SERVICE_STATE_CLOSED == g_storage.diagnostics.state)
    {
        return 0U;
    }
    if ((STORAGE_SERVICE_STATE_WAIT_CARD == g_storage.diagnostics.state) ||
        (STORAGE_SERVICE_STATE_BACKOFF == g_storage.diagnostics.state) ||
        (STORAGE_SERVICE_STATE_DEGRADED == g_storage.diagnostics.state))
    {
        return 3U;
    }
    return 1U;
}

static app_status_t storage_step (uint64_t now_us)
{
    storage_journal_record_t const * p_record = NULL;
    UINT transferred = 0U;
    FRESULT result;
    app_status_t status;

    switch (g_storage.diagnostics.state)
    {
        case STORAGE_SERVICE_STATE_MEDIA_OPEN:
            status = sd_media_open();
            if (APP_STATUS_OK != status)
            {
                return storage_fail(status, now_us, false);
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_WAIT_CARD;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_WAIT_CARD:
        {
            sd_media_diagnostics_t media;

            sd_media_get_diagnostics(&media);
            if (media.media_inserted)
            {
                g_storage.diagnostics.state = STORAGE_SERVICE_STATE_MOUNT;
            }
            return APP_STATUS_UNAVAILABLE;
        }

        case STORAGE_SERVICE_STATE_MOUNT:
            g_storage.diagnostics.mount_attempts++;
            result = f_mount(&g_storage.filesystem, "0:", 1U);
            if (FR_OK != result)
            {
                g_storage.diagnostics.mount_failures++;
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            g_storage.diagnostics.mounted = true;
            g_storage.diagnostics.writable = true;
            g_storage.consecutive_failures = 0U;
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_OPEN_BINARY;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_OPEN_BINARY:
            result = f_open(&g_storage.binary_file,
                            STORAGE_BINARY_PATH,
                            FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            g_storage.binary_open = true;
            g_storage.recover_last_good_offset = 0U;
            g_storage.recover_last_sequence = 0U;
            g_storage.recovered_record_valid = false;
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_RECOVER_SEEK;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_RECOVER_SEEK:
            result = f_lseek(&g_storage.binary_file, 0U);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_RECOVER_HEADER;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_RECOVER_HEADER:
            transferred = 0U;
            result = f_read(&g_storage.binary_file,
                            g_storage.encoded,
                            STORAGE_JOURNAL_HEADER_BYTES,
                            &transferred);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            if (0U == transferred)
            {
                g_storage.diagnostics.state = STORAGE_SERVICE_STATE_OPEN_CSV;
                return APP_STATUS_OK;
            }
            if (STORAGE_JOURNAL_HEADER_BYTES != transferred)
            {
                g_storage.diagnostics.incomplete_tails_recovered++;
                g_storage.diagnostics.state =
                    STORAGE_SERVICE_STATE_RECOVER_TRUNCATE_SEEK;
                return APP_STATUS_DATA;
            }
            g_storage.recover_payload_bytes =
                ((uint32_t) g_storage.encoded[20] << 24) |
                ((uint32_t) g_storage.encoded[21] << 16) |
                ((uint32_t) g_storage.encoded[22] << 8) |
                (uint32_t) g_storage.encoded[23];
            if (g_storage.recover_payload_bytes >
                STORAGE_JOURNAL_MAX_PAYLOAD_BYTES)
            {
                g_storage.diagnostics.corrupt_tails_recovered++;
                g_storage.diagnostics.state =
                    STORAGE_SERVICE_STATE_RECOVER_TRUNCATE_SEEK;
                return APP_STATUS_DATA;
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_RECOVER_PAYLOAD;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_RECOVER_PAYLOAD:
        {
            storage_journal_view_t view;
            uint32_t record_bytes = 0U;

            transferred = 0U;
            result = f_read(&g_storage.binary_file,
                            &g_storage.encoded[STORAGE_JOURNAL_HEADER_BYTES],
                            g_storage.recover_payload_bytes,
                            &transferred);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            if (g_storage.recover_payload_bytes != transferred)
            {
                g_storage.diagnostics.incomplete_tails_recovered++;
                g_storage.diagnostics.state =
                    STORAGE_SERVICE_STATE_RECOVER_TRUNCATE_SEEK;
                return APP_STATUS_DATA;
            }
            status = storage_journal_decode(
                g_storage.encoded,
                STORAGE_JOURNAL_HEADER_BYTES + transferred,
                &view,
                &record_bytes);
            if (APP_STATUS_OK != status)
            {
                g_storage.diagnostics.corrupt_tails_recovered++;
                g_storage.diagnostics.state =
                    STORAGE_SERVICE_STATE_RECOVER_TRUNCATE_SEEK;
                return status;
            }
            g_storage.recover_last_good_offset += record_bytes;
            g_storage.recover_last_sequence = view.sequence;
            g_storage.recovered_record_valid = true;
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_RECOVER_HEADER;
            return APP_STATUS_OK;
        }

        case STORAGE_SERVICE_STATE_RECOVER_TRUNCATE_SEEK:
            result = f_lseek(&g_storage.binary_file,
                             g_storage.recover_last_good_offset);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_RECOVER_TRUNCATE;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_RECOVER_TRUNCATE:
            result = f_truncate(&g_storage.binary_file);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_OPEN_CSV;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_OPEN_CSV:
            result = f_open(&g_storage.csv_file,
                            STORAGE_CSV_PATH,
                            FA_WRITE | FA_OPEN_APPEND);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            g_storage.csv_open = true;
            if (0U == f_size(&g_storage.csv_file))
            {
                g_storage.diagnostics.state = STORAGE_SERVICE_STATE_CSV_HEADER;
                return APP_STATUS_OK;
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_READY;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_CSV_HEADER:
        {
            char const header[] = STORAGE_CSV_HEADER;

            result = f_write(&g_storage.csv_file,
                             header,
                             (UINT) (sizeof(header) - 1U),
                             &transferred);
            if ((FR_OK != result) || (transferred != (sizeof(header) - 1U)))
            {
                return storage_fail((FR_OK == result) ? APP_STATUS_IO :
                                    storage_from_fresult(result),
                                    now_us,
                                    true);
            }
            g_storage.diagnostics.csv_bytes_written += transferred;
            g_storage.diagnostics.state =
                STORAGE_SERVICE_STATE_CSV_HEADER_SYNC;
            return APP_STATUS_OK;
        }

        case STORAGE_SERVICE_STATE_CSV_HEADER_SYNC:
            result = f_sync(&g_storage.csv_file);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_READY;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_READY:
            status = storage_journal_peek(&g_storage.queue, &p_record);
            if (APP_STATUS_UNAVAILABLE == status)
            {
                return APP_STATUS_OK;
            }
            if (APP_STATUS_OK != status)
            {
                return status;
            }
            if (g_storage.pending_binary &&
                g_storage.recovered_record_valid &&
                (p_record->sequence == g_storage.recover_last_sequence))
            {
                if (p_record->payload_bytes >= sizeof(g_storage.csv_row))
                {
                    return storage_fail(APP_STATUS_DATA, now_us, false);
                }
                memcpy(g_storage.csv_row,
                       p_record->payload,
                       p_record->payload_bytes);
                g_storage.csv_row[p_record->payload_bytes] = '\0';
                g_storage.csv_row_bytes = p_record->payload_bytes;
                g_storage.diagnostics.state = STORAGE_SERVICE_STATE_COMMIT;
                return APP_STATUS_OK;
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_ENCODE;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_ENCODE:
            status = storage_journal_peek(&g_storage.queue, &p_record);
            if (APP_STATUS_OK != status)
            {
                g_storage.diagnostics.state = STORAGE_SERVICE_STATE_READY;
                return status;
            }
            status = storage_journal_encode(p_record,
                                            g_storage.encoded,
                                            sizeof(g_storage.encoded),
                                            &g_storage.encoded_bytes);
            if (APP_STATUS_OK != status)
            {
                return storage_fail(status, now_us, false);
            }
            if (p_record->payload_bytes >= sizeof(g_storage.csv_row))
            {
                return storage_fail(APP_STATUS_DATA, now_us, false);
            }
            memcpy(g_storage.csv_row, p_record->payload, p_record->payload_bytes);
            g_storage.csv_row[p_record->payload_bytes] = '\0';
            g_storage.csv_row_bytes = p_record->payload_bytes;
            g_storage.pending_binary = true;
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_WRITE_BINARY;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_WRITE_BINARY:
            result = f_write(&g_storage.binary_file,
                             g_storage.encoded,
                             g_storage.encoded_bytes,
                             &transferred);
            if ((FR_OK != result) || (transferred != g_storage.encoded_bytes))
            {
                return storage_fail((FR_OK == result) ? APP_STATUS_IO :
                                    storage_from_fresult(result),
                                    now_us,
                                    true);
            }
            g_storage.diagnostics.binary_bytes_written += transferred;
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_SYNC_BINARY;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_SYNC_BINARY:
            result = f_sync(&g_storage.binary_file);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_COMMIT;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_WRITE_CSV:
            result = f_write(&g_storage.csv_file,
                             g_storage.csv_row,
                             g_storage.csv_row_bytes,
                             &transferred);
            if ((FR_OK != result) || (transferred != g_storage.csv_row_bytes))
            {
                return storage_fail((FR_OK == result) ? APP_STATUS_IO :
                                    storage_from_fresult(result),
                                    now_us,
                                    true);
            }
            g_storage.diagnostics.csv_bytes_written += transferred;
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_SYNC_CSV;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_SYNC_CSV:
            result = f_sync(&g_storage.csv_file);
            if (FR_OK != result)
            {
                return storage_fail(storage_from_fresult(result), now_us, true);
            }
            g_storage.diagnostics.state = STORAGE_SERVICE_STATE_READY;
            return APP_STATUS_OK;

        case STORAGE_SERVICE_STATE_COMMIT:
            status = storage_journal_peek(&g_storage.queue, &p_record);
            if (APP_STATUS_OK != status)
            {
                g_storage.diagnostics.state = STORAGE_SERVICE_STATE_READY;
                return status;
            }
            g_storage.diagnostics.last_sequence = p_record->sequence;
            status = storage_journal_commit(&g_storage.queue);
            if (APP_STATUS_OK == status)
            {
                g_storage.diagnostics.records_written++;
                g_storage.pending_binary = false;
                g_storage.encoded_bytes = 0U;
                g_storage.diagnostics.state = STORAGE_SERVICE_STATE_WRITE_CSV;
                storage_refresh_queue_metrics();
            }
            return status;

        case STORAGE_SERVICE_STATE_BACKOFF:
            if (now_us >= g_storage.retry_deadline_us)
            {
                (void) f_mount(NULL, "0:", 0U);
                (void) sd_media_close();
                g_storage.binary_open = false;
                g_storage.csv_open = false;
                g_storage.diagnostics.mounted = false;
                g_storage.diagnostics.writable = false;
                g_storage.diagnostics.retries++;
                g_storage.diagnostics.state = STORAGE_SERVICE_STATE_MEDIA_OPEN;
            }
            return APP_STATUS_UNAVAILABLE;

        case STORAGE_SERVICE_STATE_DEGRADED:
            return APP_STATUS_UNAVAILABLE;

        case STORAGE_SERVICE_STATE_CLOSED:
        default:
            return APP_STATUS_NOT_INITIALIZED;
    }
}

static app_status_t storage_fail (app_status_t status,
                                  uint64_t now_us,
                                  bool filesystem_failure)
{
    if (filesystem_failure)
    {
        g_storage.diagnostics.filesystem_failures++;
    }
    else
    {
        g_storage.diagnostics.media_failures++;
    }
    g_storage.diagnostics.last_status = status;
    g_storage.diagnostics.writable = false;
    if (g_storage.consecutive_failures < UINT8_MAX)
    {
        g_storage.consecutive_failures++;
    }
    if (g_storage.consecutive_failures >= STORAGE_MAX_FAILURES)
    {
        g_storage.diagnostics.state = STORAGE_SERVICE_STATE_DEGRADED;
    }
    else
    {
        g_storage.retry_deadline_us = now_us + STORAGE_RETRY_US;
        g_storage.diagnostics.state = STORAGE_SERVICE_STATE_BACKOFF;
    }
    return status;
}

static app_status_t storage_format_telemetry (
    app_telemetry_snapshot_t const * p_snapshot,
    char * p_output,
    uint32_t output_capacity,
    uint32_t * p_output_bytes)
{
    int written;

    if ((NULL == p_snapshot) || (NULL == p_output) ||
        (NULL == p_output_bytes) || (0U == output_capacity))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_output_bytes = 0U;
    written = snprintf(p_output,
                       output_capacity,
                       "%u,%" PRIu32 ",%" PRIu64 ",%" PRId32 ",%" PRIu32 ",%u,"
                       "%u,%u,%u,%u,%" PRId32 ",%" PRIu32 ",%u,%u,%u,%u,"
                       "%" PRIu32 ",%u,%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\r\n",
                       (unsigned int) p_snapshot->schema_version,
                       p_snapshot->sequence,
                       p_snapshot->monotonic_us,
                       p_snapshot->temperature_milli_c,
                       p_snapshot->humidity_milli_percent,
                       p_snapshot->environment_valid ? 1U : 0U,
                       (unsigned int) p_snapshot->light_raw,
                       (unsigned int) p_snapshot->light_average,
                       (unsigned int) p_snapshot->light_tenths_percent,
                       p_snapshot->light_valid ? 1U : 0U,
                       p_snapshot->wheat_count,
                       p_snapshot->inference_us,
                       (unsigned int) p_snapshot->camera_state,
                       (unsigned int) p_snapshot->ai_state,
                       (unsigned int) p_snapshot->sd_state,
                       (unsigned int) p_snapshot->network_state,
                       p_snapshot->fault_flags,
                       (unsigned int) p_snapshot->field_row,
                       (unsigned int) p_snapshot->field_column,
                       p_snapshot->mission_wheat_count,
                       p_snapshot->field_wheat_count,
                       p_snapshot->counted_frame_count);
    if ((written < 0) || ((uint32_t) written >= output_capacity))
    {
        return APP_STATUS_BUSY;
    }
    *p_output_bytes = (uint32_t) written;
    return APP_STATUS_OK;
}

static app_status_t storage_from_fresult (FRESULT result)
{
    switch (result)
    {
        case FR_OK:
            return APP_STATUS_OK;
        case FR_NOT_READY:
        case FR_NO_FILESYSTEM:
        case FR_NO_PATH:
            return APP_STATUS_UNAVAILABLE;
        case FR_TIMEOUT:
            return APP_STATUS_TIMEOUT;
        case FR_INVALID_PARAMETER:
        case FR_INVALID_DRIVE:
            return APP_STATUS_INVALID_ARGUMENT;
        case FR_INT_ERR:
        case FR_INVALID_OBJECT:
            return APP_STATUS_DATA;
        default:
            return APP_STATUS_IO;
    }
}

static void storage_refresh_queue_metrics (void)
{
    storage_journal_diagnostics_t queue;

    storage_journal_get_diagnostics(&g_storage.queue, &queue);
    g_storage.diagnostics.queued_records = queue.queued;
    g_storage.diagnostics.queue_high_water = queue.high_water;
}

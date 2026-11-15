#ifndef STORAGE_SERVICE_H_
#define STORAGE_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "app_telemetry.h"

typedef enum e_storage_service_state
{
    STORAGE_SERVICE_STATE_CLOSED = 0,
    STORAGE_SERVICE_STATE_MEDIA_OPEN,
    STORAGE_SERVICE_STATE_WAIT_CARD,
    STORAGE_SERVICE_STATE_MOUNT,
    STORAGE_SERVICE_STATE_OPEN_BINARY,
    STORAGE_SERVICE_STATE_RECOVER_SEEK,
    STORAGE_SERVICE_STATE_RECOVER_HEADER,
    STORAGE_SERVICE_STATE_RECOVER_PAYLOAD,
    STORAGE_SERVICE_STATE_RECOVER_TRUNCATE_SEEK,
    STORAGE_SERVICE_STATE_RECOVER_TRUNCATE,
    STORAGE_SERVICE_STATE_OPEN_CSV,
    STORAGE_SERVICE_STATE_CSV_HEADER,
    STORAGE_SERVICE_STATE_CSV_HEADER_SYNC,
    STORAGE_SERVICE_STATE_READY,
    STORAGE_SERVICE_STATE_ENCODE,
    STORAGE_SERVICE_STATE_WRITE_BINARY,
    STORAGE_SERVICE_STATE_SYNC_BINARY,
    STORAGE_SERVICE_STATE_WRITE_CSV,
    STORAGE_SERVICE_STATE_SYNC_CSV,
    STORAGE_SERVICE_STATE_COMMIT,
    STORAGE_SERVICE_STATE_BACKOFF,
    STORAGE_SERVICE_STATE_DEGRADED
} storage_service_state_t;

typedef struct st_storage_service_diagnostics
{
    storage_service_state_t state;
    app_status_t last_status;
    uint32_t last_sequence;
    uint32_t records_written;
    uint32_t binary_bytes_written;
    uint32_t csv_bytes_written;
    uint32_t mount_attempts;
    uint32_t mount_failures;
    uint32_t media_failures;
    uint32_t filesystem_failures;
    uint32_t corrupt_tails_recovered;
    uint32_t incomplete_tails_recovered;
    uint32_t retries;
    uint32_t queue_high_water;
    uint8_t queued_records;
    bool initialized;
    bool mounted;
    bool writable;
} storage_service_diagnostics_t;

app_status_t storage_service_init(uint64_t now_us);
app_status_t storage_service_enqueue_telemetry(
    app_telemetry_snapshot_t const * p_snapshot);
app_status_t storage_service_process(uint64_t now_us, bool allow_filesystem_io);
app_status_t storage_service_close(void);
void storage_service_get_diagnostics(storage_service_diagnostics_t * p_diagnostics);
uint8_t storage_service_telemetry_state(void);

#endif

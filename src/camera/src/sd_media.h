#ifndef SD_MEDIA_H_
#define SD_MEDIA_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define SD_MEDIA_SECTOR_BYTES          (512U)
#define SD_MEDIA_CACHE_LINE_BYTES      (32U)
#define SD_MEDIA_TRANSFER_TIMEOUT_US   (250000U)
#define SD_MEDIA_MAX_SECTORS_PER_CALL  (8U)

typedef enum e_sd_media_state
{
    SD_MEDIA_STATE_CLOSED = 0,
    SD_MEDIA_STATE_NO_CARD,
    SD_MEDIA_STATE_INITIALIZING,
    SD_MEDIA_STATE_READY,
    SD_MEDIA_STATE_BUSY,
    SD_MEDIA_STATE_DEGRADED
} sd_media_state_t;

typedef enum e_sd_media_event_flag
{
    SD_MEDIA_EVENT_NONE      = 0U,
    SD_MEDIA_EVENT_COMPLETE  = 1U << 0,
    SD_MEDIA_EVENT_ERROR     = 1U << 1,
    SD_MEDIA_EVENT_INSERTED  = 1U << 2,
    SD_MEDIA_EVENT_REMOVED   = 1U << 3
} sd_media_event_flag_t;

typedef struct st_sd_media_diagnostics
{
    uint32_t sector_size_bytes;
    uint32_t sector_count;
    uint32_t operations_completed;
    uint32_t operation_errors;
    uint32_t operation_timeouts;
    uint32_t insertions;
    uint32_t removals;
    uint32_t bounce_transfers;
    uint32_t direct_transfers;
    int32_t  last_fsp_error;
    sd_media_state_t state;
    bool opened;
    bool initialized;
    bool media_inserted;
    bool write_protected;
} sd_media_diagnostics_t;

app_status_t sd_media_open(void);
app_status_t sd_media_initialize(void);
app_status_t sd_media_read(uint8_t * p_destination,
                           uint32_t sector,
                           uint32_t count);
app_status_t sd_media_write(uint8_t const * p_source,
                            uint32_t sector,
                            uint32_t count);
app_status_t sd_media_sync(void);
app_status_t sd_media_close(void);
void sd_media_process(void);
void sd_media_get_diagnostics(sd_media_diagnostics_t * p_diagnostics);
bool sd_media_buffer_is_aligned(void const * p_buffer);
app_status_t sd_media_evaluate_operation(uint32_t events, uint64_t elapsed_us);

#if !defined(SD_MEDIA_HOST_TEST)
 #include "rm_block_media_api.h"
void sd_media_callback(rm_block_media_callback_args_t * p_args);
#else
void sd_media_host_inject_events(uint32_t events);
#endif

#endif

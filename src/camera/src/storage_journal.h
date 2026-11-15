#ifndef STORAGE_JOURNAL_H_
#define STORAGE_JOURNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define STORAGE_JOURNAL_SCHEMA_VERSION       (1U)
#define STORAGE_JOURNAL_HEADER_BYTES         (32U)
#define STORAGE_JOURNAL_MAX_PAYLOAD_BYTES    (1024U)
#define STORAGE_JOURNAL_QUEUE_CAPACITY       (8U)

#define STORAGE_CONFIG_SCHEMA_VERSION        (1U)
#define STORAGE_CONFIG_HEADER_BYTES          (24U)
#define STORAGE_CONFIG_MAX_PAYLOAD_BYTES     (512U)

typedef enum e_storage_record_type
{
    STORAGE_RECORD_TELEMETRY = 1,
    STORAGE_RECORD_EVENT,
    STORAGE_RECORD_FAULT,
    STORAGE_RECORD_MISSION,
    STORAGE_RECORD_CONFIGURATION
} storage_record_type_t;

typedef struct st_storage_journal_record
{
    uint16_t type;
    uint32_t sequence;
    uint64_t monotonic_us;
    uint32_t payload_bytes;
    uint32_t payload_crc32;
    uint8_t  payload[STORAGE_JOURNAL_MAX_PAYLOAD_BYTES];
} storage_journal_record_t;

typedef struct st_storage_journal_view
{
    uint16_t        schema_version;
    uint16_t        type;
    uint32_t        sequence;
    uint64_t        monotonic_us;
    uint32_t        payload_bytes;
    uint32_t        payload_crc32;
    uint8_t const * p_payload;
} storage_journal_view_t;

typedef struct st_storage_journal_diagnostics
{
    uint32_t enqueued;
    uint32_t committed;
    uint32_t rejected_full;
    uint32_t payload_bytes_enqueued;
    uint32_t high_water;
    uint8_t  queued;
    app_status_t last_status;
} storage_journal_diagnostics_t;

typedef struct st_storage_journal_queue
{
    storage_journal_record_t records[STORAGE_JOURNAL_QUEUE_CAPACITY];
    storage_journal_diagnostics_t diagnostics;
    uint8_t head;
    uint8_t tail;
    uint8_t used;
} storage_journal_queue_t;

typedef struct st_storage_config_view
{
    uint16_t        schema_version;
    uint32_t        generation;
    uint32_t        payload_bytes;
    uint32_t        payload_crc32;
    uint8_t const * p_payload;
    uint8_t         slot_index;
} storage_config_view_t;

void storage_journal_init(storage_journal_queue_t * p_queue);
app_status_t storage_journal_enqueue(storage_journal_queue_t * p_queue,
                                     uint16_t type,
                                     uint32_t sequence,
                                     uint64_t monotonic_us,
                                     uint8_t const * p_payload,
                                     uint32_t payload_bytes);
app_status_t storage_journal_peek(storage_journal_queue_t const * p_queue,
                                  storage_journal_record_t const ** pp_record);
app_status_t storage_journal_commit(storage_journal_queue_t * p_queue);
app_status_t storage_journal_encode(storage_journal_record_t const * p_record,
                                    uint8_t * p_output,
                                    uint32_t output_capacity,
                                    uint32_t * p_output_bytes);
app_status_t storage_journal_decode(uint8_t const * p_input,
                                    uint32_t input_bytes,
                                    storage_journal_view_t * p_view,
                                    uint32_t * p_record_bytes);
void storage_journal_get_diagnostics(storage_journal_queue_t const * p_queue,
                                     storage_journal_diagnostics_t * p_diagnostics);

app_status_t storage_config_encode(uint32_t generation,
                                   uint8_t const * p_payload,
                                   uint32_t payload_bytes,
                                   uint8_t * p_output,
                                   uint32_t output_capacity,
                                   uint32_t * p_output_bytes);
app_status_t storage_config_validate(uint8_t const * p_slot,
                                     uint32_t slot_bytes,
                                     storage_config_view_t * p_view);
app_status_t storage_config_select(uint8_t const * p_slot_a,
                                   uint32_t slot_a_bytes,
                                   uint8_t const * p_slot_b,
                                   uint32_t slot_b_bytes,
                                   storage_config_view_t * p_selected);

#endif /* STORAGE_JOURNAL_H_ */

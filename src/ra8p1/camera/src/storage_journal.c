#include "storage_journal.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "wire_protocol.h"

#define STORAGE_JOURNAL_MAGIC_U32  (0x52384A52UL) /* R8JR */
#define STORAGE_CONFIG_MAGIC_U32   (0x52384346UL) /* R8CF */

#define STORAGE_JOURNAL_MAGIC_OFFSET          (0U)
#define STORAGE_JOURNAL_SCHEMA_OFFSET         (4U)
#define STORAGE_JOURNAL_TYPE_OFFSET           (6U)
#define STORAGE_JOURNAL_SEQUENCE_OFFSET       (8U)
#define STORAGE_JOURNAL_MONOTONIC_OFFSET      (12U)
#define STORAGE_JOURNAL_PAYLOAD_BYTES_OFFSET  (20U)
#define STORAGE_JOURNAL_PAYLOAD_CRC_OFFSET    (24U)
#define STORAGE_JOURNAL_HEADER_CRC_OFFSET     (28U)

#define STORAGE_CONFIG_MAGIC_OFFSET          (0U)
#define STORAGE_CONFIG_SCHEMA_OFFSET         (4U)
#define STORAGE_CONFIG_RESERVED_OFFSET       (6U)
#define STORAGE_CONFIG_GENERATION_OFFSET     (8U)
#define STORAGE_CONFIG_PAYLOAD_BYTES_OFFSET  (12U)
#define STORAGE_CONFIG_PAYLOAD_CRC_OFFSET    (16U)
#define STORAGE_CONFIG_HEADER_CRC_OFFSET     (20U)

static uint16_t storage_read_u16_be(uint8_t const * p_data);
static uint32_t storage_read_u32_be(uint8_t const * p_data);
static uint64_t storage_read_u64_be(uint8_t const * p_data);
static void storage_write_u16_be(uint8_t * p_data, uint16_t value);
static void storage_write_u32_be(uint8_t * p_data, uint32_t value);
static void storage_write_u64_be(uint8_t * p_data, uint64_t value);
static bool storage_generation_is_newer(uint32_t candidate, uint32_t reference);
static app_status_t storage_journal_record_status(storage_journal_queue_t * p_queue,
                                                  app_status_t status);

void storage_journal_init (storage_journal_queue_t * p_queue)
{
    if (NULL != p_queue)
    {
        *p_queue = (storage_journal_queue_t) {0};
        p_queue->diagnostics.last_status = APP_STATUS_OK;
    }
}

app_status_t storage_journal_enqueue (storage_journal_queue_t * p_queue,
                                      uint16_t type,
                                      uint32_t sequence,
                                      uint64_t monotonic_us,
                                      uint8_t const * p_payload,
                                      uint32_t payload_bytes)
{
    storage_journal_record_t * p_record;

    if ((NULL == p_queue) || (0U == type) ||
        ((0U < payload_bytes) && (NULL == p_payload)) ||
        (payload_bytes > STORAGE_JOURNAL_MAX_PAYLOAD_BYTES))
    {
        return storage_journal_record_status(p_queue, APP_STATUS_INVALID_ARGUMENT);
    }
    if (STORAGE_JOURNAL_QUEUE_CAPACITY == p_queue->used)
    {
        p_queue->diagnostics.rejected_full++;
        return storage_journal_record_status(p_queue, APP_STATUS_BUSY);
    }

    p_record = &p_queue->records[p_queue->head];
    *p_record = (storage_journal_record_t) {0};
    p_record->type = type;
    p_record->sequence = sequence;
    p_record->monotonic_us = monotonic_us;
    p_record->payload_bytes = payload_bytes;
    p_record->payload_crc32 = wire_protocol_crc32(p_payload, payload_bytes);
    if (0U < payload_bytes)
    {
        (void) memcpy(p_record->payload, p_payload, payload_bytes);
    }

    p_queue->head = (uint8_t) ((p_queue->head + 1U) % STORAGE_JOURNAL_QUEUE_CAPACITY);
    p_queue->used++;
    p_queue->diagnostics.enqueued++;
    if (payload_bytes > (UINT32_MAX - p_queue->diagnostics.payload_bytes_enqueued))
    {
        p_queue->diagnostics.payload_bytes_enqueued = UINT32_MAX;
    }
    else
    {
        p_queue->diagnostics.payload_bytes_enqueued += payload_bytes;
    }
    p_queue->diagnostics.queued = p_queue->used;
    if (p_queue->used > p_queue->diagnostics.high_water)
    {
        p_queue->diagnostics.high_water = p_queue->used;
    }

    return storage_journal_record_status(p_queue, APP_STATUS_OK);
}

app_status_t storage_journal_peek (storage_journal_queue_t const * p_queue,
                                   storage_journal_record_t const ** pp_record)
{
    if ((NULL == p_queue) || (NULL == pp_record))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *pp_record = NULL;
    if (0U == p_queue->used)
    {
        return APP_STATUS_UNAVAILABLE;
    }

    *pp_record = &p_queue->records[p_queue->tail];
    return APP_STATUS_OK;
}

app_status_t storage_journal_commit (storage_journal_queue_t * p_queue)
{
    if (NULL == p_queue)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (0U == p_queue->used)
    {
        return storage_journal_record_status(p_queue, APP_STATUS_UNAVAILABLE);
    }

    p_queue->records[p_queue->tail] = (storage_journal_record_t) {0};
    p_queue->tail = (uint8_t) ((p_queue->tail + 1U) % STORAGE_JOURNAL_QUEUE_CAPACITY);
    p_queue->used--;
    p_queue->diagnostics.committed++;
    p_queue->diagnostics.queued = p_queue->used;

    return storage_journal_record_status(p_queue, APP_STATUS_OK);
}

app_status_t storage_journal_encode (storage_journal_record_t const * p_record,
                                     uint8_t * p_output,
                                     uint32_t output_capacity,
                                     uint32_t * p_output_bytes)
{
    uint32_t record_bytes;
    uint32_t payload_crc32;
    uint32_t header_crc32;

    if (NULL != p_output_bytes)
    {
        *p_output_bytes = 0U;
    }
    if ((NULL == p_record) || (NULL == p_output) || (NULL == p_output_bytes) ||
        (0U == p_record->type) ||
        (p_record->payload_bytes > STORAGE_JOURNAL_MAX_PAYLOAD_BYTES))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    record_bytes = STORAGE_JOURNAL_HEADER_BYTES + p_record->payload_bytes;
    if (output_capacity < record_bytes)
    {
        return APP_STATUS_BUSY;
    }

    payload_crc32 = wire_protocol_crc32(p_record->payload, p_record->payload_bytes);
    if (payload_crc32 != p_record->payload_crc32)
    {
        return APP_STATUS_DATA;
    }

    storage_write_u32_be(&p_output[STORAGE_JOURNAL_MAGIC_OFFSET],
                         STORAGE_JOURNAL_MAGIC_U32);
    storage_write_u16_be(&p_output[STORAGE_JOURNAL_SCHEMA_OFFSET],
                         (uint16_t) STORAGE_JOURNAL_SCHEMA_VERSION);
    storage_write_u16_be(&p_output[STORAGE_JOURNAL_TYPE_OFFSET], p_record->type);
    storage_write_u32_be(&p_output[STORAGE_JOURNAL_SEQUENCE_OFFSET], p_record->sequence);
    storage_write_u64_be(&p_output[STORAGE_JOURNAL_MONOTONIC_OFFSET],
                         p_record->monotonic_us);
    storage_write_u32_be(&p_output[STORAGE_JOURNAL_PAYLOAD_BYTES_OFFSET],
                         p_record->payload_bytes);
    storage_write_u32_be(&p_output[STORAGE_JOURNAL_PAYLOAD_CRC_OFFSET], payload_crc32);
    header_crc32 = wire_protocol_crc32(p_output, STORAGE_JOURNAL_HEADER_CRC_OFFSET);
    storage_write_u32_be(&p_output[STORAGE_JOURNAL_HEADER_CRC_OFFSET], header_crc32);
    if (0U < p_record->payload_bytes)
    {
        (void) memcpy(&p_output[STORAGE_JOURNAL_HEADER_BYTES],
                      p_record->payload,
                      p_record->payload_bytes);
    }
    *p_output_bytes = record_bytes;

    return APP_STATUS_OK;
}

app_status_t storage_journal_decode (uint8_t const * p_input,
                                     uint32_t input_bytes,
                                     storage_journal_view_t * p_view,
                                     uint32_t * p_record_bytes)
{
    uint32_t payload_bytes;
    uint32_t record_bytes;
    uint32_t expected_crc32;

    if ((NULL == p_input) || (NULL == p_view) || (NULL == p_record_bytes))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_view = (storage_journal_view_t) {0};
    *p_record_bytes = 0U;
    if (input_bytes < STORAGE_JOURNAL_HEADER_BYTES)
    {
        return APP_STATUS_BUSY;
    }
    if ((STORAGE_JOURNAL_MAGIC_U32 !=
         storage_read_u32_be(&p_input[STORAGE_JOURNAL_MAGIC_OFFSET])) ||
        ((uint16_t) STORAGE_JOURNAL_SCHEMA_VERSION !=
         storage_read_u16_be(&p_input[STORAGE_JOURNAL_SCHEMA_OFFSET])) ||
        (0U == storage_read_u16_be(&p_input[STORAGE_JOURNAL_TYPE_OFFSET])))
    {
        return APP_STATUS_PROTOCOL;
    }

    expected_crc32 = wire_protocol_crc32(p_input, STORAGE_JOURNAL_HEADER_CRC_OFFSET);
    if (expected_crc32 != storage_read_u32_be(&p_input[STORAGE_JOURNAL_HEADER_CRC_OFFSET]))
    {
        return APP_STATUS_DATA;
    }

    payload_bytes = storage_read_u32_be(&p_input[STORAGE_JOURNAL_PAYLOAD_BYTES_OFFSET]);
    if (payload_bytes > STORAGE_JOURNAL_MAX_PAYLOAD_BYTES)
    {
        return APP_STATUS_PROTOCOL;
    }
    record_bytes = STORAGE_JOURNAL_HEADER_BYTES + payload_bytes;
    if (input_bytes < record_bytes)
    {
        return APP_STATUS_BUSY;
    }
    expected_crc32 = wire_protocol_crc32(&p_input[STORAGE_JOURNAL_HEADER_BYTES],
                                         payload_bytes);
    if (expected_crc32 != storage_read_u32_be(&p_input[STORAGE_JOURNAL_PAYLOAD_CRC_OFFSET]))
    {
        return APP_STATUS_DATA;
    }

    p_view->schema_version = storage_read_u16_be(&p_input[STORAGE_JOURNAL_SCHEMA_OFFSET]);
    p_view->type = storage_read_u16_be(&p_input[STORAGE_JOURNAL_TYPE_OFFSET]);
    p_view->sequence = storage_read_u32_be(&p_input[STORAGE_JOURNAL_SEQUENCE_OFFSET]);
    p_view->monotonic_us = storage_read_u64_be(&p_input[STORAGE_JOURNAL_MONOTONIC_OFFSET]);
    p_view->payload_bytes = payload_bytes;
    p_view->payload_crc32 = expected_crc32;
    p_view->p_payload = &p_input[STORAGE_JOURNAL_HEADER_BYTES];
    *p_record_bytes = record_bytes;

    return APP_STATUS_OK;
}

void storage_journal_get_diagnostics (storage_journal_queue_t const * p_queue,
                                      storage_journal_diagnostics_t * p_diagnostics)
{
    if ((NULL != p_queue) && (NULL != p_diagnostics))
    {
        *p_diagnostics = p_queue->diagnostics;
    }
}

app_status_t storage_config_encode (uint32_t generation,
                                    uint8_t const * p_payload,
                                    uint32_t payload_bytes,
                                    uint8_t * p_output,
                                    uint32_t output_capacity,
                                    uint32_t * p_output_bytes)
{
    uint32_t slot_bytes;
    uint32_t payload_crc32;
    uint32_t header_crc32;

    if (NULL != p_output_bytes)
    {
        *p_output_bytes = 0U;
    }
    if ((NULL == p_output) || (NULL == p_output_bytes) ||
        ((0U < payload_bytes) && (NULL == p_payload)) ||
        (payload_bytes > STORAGE_CONFIG_MAX_PAYLOAD_BYTES))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    slot_bytes = STORAGE_CONFIG_HEADER_BYTES + payload_bytes;
    if (output_capacity < slot_bytes)
    {
        return APP_STATUS_BUSY;
    }

    payload_crc32 = wire_protocol_crc32(p_payload, payload_bytes);
    storage_write_u32_be(&p_output[STORAGE_CONFIG_MAGIC_OFFSET], STORAGE_CONFIG_MAGIC_U32);
    storage_write_u16_be(&p_output[STORAGE_CONFIG_SCHEMA_OFFSET],
                         (uint16_t) STORAGE_CONFIG_SCHEMA_VERSION);
    storage_write_u16_be(&p_output[STORAGE_CONFIG_RESERVED_OFFSET], 0U);
    storage_write_u32_be(&p_output[STORAGE_CONFIG_GENERATION_OFFSET], generation);
    storage_write_u32_be(&p_output[STORAGE_CONFIG_PAYLOAD_BYTES_OFFSET], payload_bytes);
    storage_write_u32_be(&p_output[STORAGE_CONFIG_PAYLOAD_CRC_OFFSET], payload_crc32);
    header_crc32 = wire_protocol_crc32(p_output, STORAGE_CONFIG_HEADER_CRC_OFFSET);
    storage_write_u32_be(&p_output[STORAGE_CONFIG_HEADER_CRC_OFFSET], header_crc32);
    if (0U < payload_bytes)
    {
        (void) memcpy(&p_output[STORAGE_CONFIG_HEADER_BYTES], p_payload, payload_bytes);
    }
    *p_output_bytes = slot_bytes;

    return APP_STATUS_OK;
}

app_status_t storage_config_validate (uint8_t const * p_slot,
                                      uint32_t slot_bytes,
                                      storage_config_view_t * p_view)
{
    uint32_t payload_bytes;
    uint32_t expected_crc32;

    if ((NULL == p_slot) || (NULL == p_view))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_view = (storage_config_view_t) {0};
    if (slot_bytes < STORAGE_CONFIG_HEADER_BYTES)
    {
        return APP_STATUS_BUSY;
    }
    if ((STORAGE_CONFIG_MAGIC_U32 !=
         storage_read_u32_be(&p_slot[STORAGE_CONFIG_MAGIC_OFFSET])) ||
        ((uint16_t) STORAGE_CONFIG_SCHEMA_VERSION !=
         storage_read_u16_be(&p_slot[STORAGE_CONFIG_SCHEMA_OFFSET])) ||
        (0U != storage_read_u16_be(&p_slot[STORAGE_CONFIG_RESERVED_OFFSET])))
    {
        return APP_STATUS_PROTOCOL;
    }

    expected_crc32 = wire_protocol_crc32(p_slot, STORAGE_CONFIG_HEADER_CRC_OFFSET);
    if (expected_crc32 != storage_read_u32_be(&p_slot[STORAGE_CONFIG_HEADER_CRC_OFFSET]))
    {
        return APP_STATUS_DATA;
    }

    payload_bytes = storage_read_u32_be(&p_slot[STORAGE_CONFIG_PAYLOAD_BYTES_OFFSET]);
    if (payload_bytes > STORAGE_CONFIG_MAX_PAYLOAD_BYTES)
    {
        return APP_STATUS_PROTOCOL;
    }
    if (slot_bytes < (STORAGE_CONFIG_HEADER_BYTES + payload_bytes))
    {
        return APP_STATUS_BUSY;
    }
    expected_crc32 = wire_protocol_crc32(&p_slot[STORAGE_CONFIG_HEADER_BYTES], payload_bytes);
    if (expected_crc32 != storage_read_u32_be(&p_slot[STORAGE_CONFIG_PAYLOAD_CRC_OFFSET]))
    {
        return APP_STATUS_DATA;
    }

    p_view->schema_version = storage_read_u16_be(&p_slot[STORAGE_CONFIG_SCHEMA_OFFSET]);
    p_view->generation = storage_read_u32_be(&p_slot[STORAGE_CONFIG_GENERATION_OFFSET]);
    p_view->payload_bytes = payload_bytes;
    p_view->payload_crc32 = expected_crc32;
    p_view->p_payload = &p_slot[STORAGE_CONFIG_HEADER_BYTES];

    return APP_STATUS_OK;
}

app_status_t storage_config_select (uint8_t const * p_slot_a,
                                    uint32_t slot_a_bytes,
                                    uint8_t const * p_slot_b,
                                    uint32_t slot_b_bytes,
                                    storage_config_view_t * p_selected)
{
    storage_config_view_t slot_a;
    storage_config_view_t slot_b;
    app_status_t const status_a = storage_config_validate(p_slot_a, slot_a_bytes, &slot_a);
    app_status_t const status_b = storage_config_validate(p_slot_b, slot_b_bytes, &slot_b);

    if (NULL == p_selected)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    *p_selected = (storage_config_view_t) {0};

    if ((APP_STATUS_OK != status_a) && (APP_STATUS_OK != status_b))
    {
        return APP_STATUS_DATA;
    }
    if ((APP_STATUS_OK == status_b) &&
        ((APP_STATUS_OK != status_a) ||
         storage_generation_is_newer(slot_b.generation, slot_a.generation)))
    {
        *p_selected = slot_b;
        p_selected->slot_index = 1U;
    }
    else
    {
        *p_selected = slot_a;
        p_selected->slot_index = 0U;
    }

    return APP_STATUS_OK;
}

static uint16_t storage_read_u16_be (uint8_t const * p_data)
{
    return (uint16_t) (((uint16_t) p_data[0] << 8U) | (uint16_t) p_data[1]);
}

static uint32_t storage_read_u32_be (uint8_t const * p_data)
{
    return ((uint32_t) p_data[0] << 24U) |
           ((uint32_t) p_data[1] << 16U) |
           ((uint32_t) p_data[2] << 8U) |
           (uint32_t) p_data[3];
}

static uint64_t storage_read_u64_be (uint8_t const * p_data)
{
    return ((uint64_t) storage_read_u32_be(p_data) << 32U) |
           (uint64_t) storage_read_u32_be(&p_data[4]);
}

static void storage_write_u16_be (uint8_t * p_data, uint16_t value)
{
    p_data[0] = (uint8_t) (value >> 8U);
    p_data[1] = (uint8_t) value;
}

static void storage_write_u32_be (uint8_t * p_data, uint32_t value)
{
    p_data[0] = (uint8_t) (value >> 24U);
    p_data[1] = (uint8_t) (value >> 16U);
    p_data[2] = (uint8_t) (value >> 8U);
    p_data[3] = (uint8_t) value;
}

static void storage_write_u64_be (uint8_t * p_data, uint64_t value)
{
    storage_write_u32_be(p_data, (uint32_t) (value >> 32U));
    storage_write_u32_be(&p_data[4], (uint32_t) value);
}

static bool storage_generation_is_newer (uint32_t candidate, uint32_t reference)
{
    uint32_t const distance = candidate - reference;

    return (0U != distance) && (distance < 0x80000000UL);
}

static app_status_t storage_journal_record_status (storage_journal_queue_t * p_queue,
                                                   app_status_t status)
{
    if (NULL != p_queue)
    {
        p_queue->diagnostics.last_status = status;
    }
    return status;
}

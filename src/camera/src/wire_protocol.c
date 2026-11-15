#include "wire_protocol.h"

#include <stddef.h>
#include <string.h>

static uint16_t wire_protocol_read_u16_be(uint8_t const * p_data);
static uint32_t wire_protocol_read_u32_be(uint8_t const * p_data);
static void     wire_protocol_write_u16_be(uint8_t * p_data, uint16_t value);
static void     wire_protocol_write_u32_be(uint8_t * p_data, uint32_t value);
static app_status_t wire_protocol_process_buffer(wire_protocol_decoder_t * p_decoder,
                                                 wire_protocol_frame_handler_t handler,
                                                 void * p_context);
static app_status_t wire_protocol_record(wire_protocol_decoder_t * p_decoder,
                                         app_status_t status);

app_status_t wire_protocol_decoder_init (wire_protocol_decoder_t * p_decoder,
                                         uint8_t * p_storage,
                                         uint32_t capacity)
{
    if ((NULL == p_decoder) || (NULL == p_storage) ||
        (capacity < WIRE_PROTOCOL_MAX_FRAME_BYTES))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_decoder = (wire_protocol_decoder_t) {0};
    p_decoder->p_storage = p_storage;
    p_decoder->capacity  = capacity;
    p_decoder->diagnostics.last_status = APP_STATUS_OK;

    return APP_STATUS_OK;
}

app_status_t wire_protocol_decoder_feed (wire_protocol_decoder_t * p_decoder,
                                         uint8_t const * p_data,
                                         uint32_t data_bytes,
                                         uint32_t * p_consumed_bytes,
                                         wire_protocol_frame_handler_t handler,
                                         void * p_context)
{
    uint32_t offset = 0U;
    app_status_t status;

    if ((NULL == p_decoder) || (NULL == p_decoder->p_storage) ||
        (NULL == p_consumed_bytes) || (NULL == handler) ||
        ((0U < data_bytes) && (NULL == p_data)))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_consumed_bytes = 0U;
    while (offset < data_bytes)
    {
        uint32_t available;
        uint32_t copy_bytes;

        status = wire_protocol_process_buffer(p_decoder, handler, p_context);
        if (APP_STATUS_OK != status)
        {
            return status;
        }

        available = p_decoder->capacity - p_decoder->used;
        if (0U == available)
        {
            p_decoder->diagnostics.buffer_overflows++;
            wire_protocol_decoder_reset(p_decoder);
            return wire_protocol_record(p_decoder, APP_STATUS_BUSY);
        }

        copy_bytes = data_bytes - offset;
        if (copy_bytes > available)
        {
            copy_bytes = available;
        }
        (void) memcpy(&p_decoder->p_storage[p_decoder->used], &p_data[offset], copy_bytes);
        p_decoder->used += copy_bytes;
        offset += copy_bytes;
        *p_consumed_bytes = offset;
        if (copy_bytes > (UINT32_MAX - p_decoder->diagnostics.bytes_received))
        {
            p_decoder->diagnostics.bytes_received = UINT32_MAX;
        }
        else
        {
            p_decoder->diagnostics.bytes_received += copy_bytes;
        }
        p_decoder->diagnostics.buffered_bytes = p_decoder->used;
        if (p_decoder->used > p_decoder->diagnostics.buffer_high_water)
        {
            p_decoder->diagnostics.buffer_high_water = p_decoder->used;
        }
    }

    status = wire_protocol_process_buffer(p_decoder, handler, p_context);
    return wire_protocol_record(p_decoder, status);
}

void wire_protocol_decoder_reset (wire_protocol_decoder_t * p_decoder)
{
    if (NULL != p_decoder)
    {
        p_decoder->used = 0U;
        p_decoder->diagnostics.buffered_bytes = 0U;
    }
}

void wire_protocol_decoder_get_diagnostics (wire_protocol_decoder_t const * p_decoder,
                                            wire_protocol_diagnostics_t * p_diagnostics)
{
    if ((NULL != p_decoder) && (NULL != p_diagnostics))
    {
        *p_diagnostics = p_decoder->diagnostics;
    }
}

app_status_t wire_protocol_encode (uint16_t type,
                                   uint32_t sequence,
                                   uint32_t flags,
                                   uint8_t const * p_payload,
                                   uint32_t payload_bytes,
                                   uint8_t * p_output,
                                   uint32_t output_capacity,
                                   uint32_t * p_output_bytes)
{
    uint32_t frame_bytes;
    uint32_t payload_crc32;

    if ((NULL == p_output) || (NULL == p_output_bytes) ||
        ((0U < payload_bytes) && (NULL == p_payload)))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (payload_bytes > RA8P_PROTOCOL_MAX_PAYLOAD_BYTES)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    frame_bytes = RA8P_PROTOCOL_HEADER_BYTES + payload_bytes;
    if (output_capacity < frame_bytes)
    {
        return APP_STATUS_BUSY;
    }

    payload_crc32 = wire_protocol_crc32(p_payload, payload_bytes);
    wire_protocol_write_u32_be(&p_output[RA8P_HEADER_MAGIC_OFFSET], RA8P_PROTOCOL_MAGIC_U32);
    wire_protocol_write_u16_be(&p_output[RA8P_HEADER_VERSION_OFFSET],
                               (uint16_t) RA8P_PROTOCOL_VERSION);
    wire_protocol_write_u16_be(&p_output[RA8P_HEADER_TYPE_OFFSET], type);
    wire_protocol_write_u32_be(&p_output[RA8P_HEADER_SEQUENCE_OFFSET], sequence);
    wire_protocol_write_u32_be(&p_output[RA8P_HEADER_PAYLOAD_BYTES_OFFSET], payload_bytes);
    wire_protocol_write_u32_be(&p_output[RA8P_HEADER_PAYLOAD_CRC32_OFFSET], payload_crc32);
    wire_protocol_write_u32_be(&p_output[RA8P_HEADER_FLAGS_OFFSET], flags);
    if (0U < payload_bytes)
    {
        (void) memcpy(&p_output[RA8P_PROTOCOL_HEADER_BYTES], p_payload, payload_bytes);
    }
    *p_output_bytes = frame_bytes;

    return APP_STATUS_OK;
}

uint32_t wire_protocol_crc32 (uint8_t const * p_data, uint32_t length)
{
    uint32_t crc = UINT32_MAX;
    uint32_t index;

    if ((NULL == p_data) && (0U < length))
    {
        return 0U;
    }
    for (index = 0U; index < length; index++)
    {
        uint32_t bit;

        crc ^= p_data[index];
        for (bit = 0U; bit < 8U; bit++)
        {
            uint32_t const mask = (uint32_t) (0U - (crc & 1U));

            crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}

static app_status_t wire_protocol_process_buffer (wire_protocol_decoder_t * p_decoder,
                                                  wire_protocol_frame_handler_t handler,
                                                  void * p_context)
{
    while (p_decoder->used >= RA8P_PROTOCOL_HEADER_BYTES)
    {
        uint32_t const magic = wire_protocol_read_u32_be(
            &p_decoder->p_storage[RA8P_HEADER_MAGIC_OFFSET]);
        uint16_t const version = wire_protocol_read_u16_be(
            &p_decoder->p_storage[RA8P_HEADER_VERSION_OFFSET]);
        uint32_t const payload_bytes = wire_protocol_read_u32_be(
            &p_decoder->p_storage[RA8P_HEADER_PAYLOAD_BYTES_OFFSET]);
        uint32_t frame_bytes;
        wire_protocol_frame_t frame;
        app_status_t callback_status;

        if (RA8P_PROTOCOL_MAGIC_U32 != magic)
        {
            p_decoder->diagnostics.magic_errors++;
            wire_protocol_decoder_reset(p_decoder);
            return wire_protocol_record(p_decoder, APP_STATUS_PROTOCOL);
        }
        if ((uint16_t) RA8P_PROTOCOL_VERSION != version)
        {
            p_decoder->diagnostics.version_errors++;
            wire_protocol_decoder_reset(p_decoder);
            return wire_protocol_record(p_decoder, APP_STATUS_PROTOCOL);
        }
        if (payload_bytes > RA8P_PROTOCOL_MAX_PAYLOAD_BYTES)
        {
            p_decoder->diagnostics.oversized_frames++;
            wire_protocol_decoder_reset(p_decoder);
            return wire_protocol_record(p_decoder, APP_STATUS_PROTOCOL);
        }
        frame_bytes = RA8P_PROTOCOL_HEADER_BYTES + payload_bytes;
        if (p_decoder->used < frame_bytes)
        {
            return APP_STATUS_OK;
        }

        frame.version = version;
        frame.type = wire_protocol_read_u16_be(
            &p_decoder->p_storage[RA8P_HEADER_TYPE_OFFSET]);
        frame.sequence = wire_protocol_read_u32_be(
            &p_decoder->p_storage[RA8P_HEADER_SEQUENCE_OFFSET]);
        frame.payload_bytes = payload_bytes;
        frame.payload_crc32 = wire_protocol_read_u32_be(
            &p_decoder->p_storage[RA8P_HEADER_PAYLOAD_CRC32_OFFSET]);
        frame.flags = wire_protocol_read_u32_be(
            &p_decoder->p_storage[RA8P_HEADER_FLAGS_OFFSET]);
        frame.p_payload = &p_decoder->p_storage[RA8P_PROTOCOL_HEADER_BYTES];

        if (frame.payload_crc32 != wire_protocol_crc32(frame.p_payload, frame.payload_bytes))
        {
            p_decoder->diagnostics.crc_errors++;
            wire_protocol_decoder_reset(p_decoder);
            return wire_protocol_record(p_decoder, APP_STATUS_DATA);
        }

        callback_status = handler(&frame, p_context);
        if (APP_STATUS_BUSY == callback_status)
        {
            p_decoder->diagnostics.callback_busy++;
            return wire_protocol_record(p_decoder, APP_STATUS_BUSY);
        }

        p_decoder->used -= frame_bytes;
        if (0U < p_decoder->used)
        {
            (void) memmove(p_decoder->p_storage,
                           &p_decoder->p_storage[frame_bytes],
                           p_decoder->used);
        }
        p_decoder->diagnostics.buffered_bytes = p_decoder->used;
        p_decoder->diagnostics.frames_received++;
        if (APP_STATUS_OK != callback_status)
        {
            p_decoder->diagnostics.callback_errors++;
            return wire_protocol_record(p_decoder, callback_status);
        }
    }

    return APP_STATUS_OK;
}

static uint16_t wire_protocol_read_u16_be (uint8_t const * p_data)
{
    return (uint16_t) (((uint16_t) p_data[0] << 8U) | (uint16_t) p_data[1]);
}

static uint32_t wire_protocol_read_u32_be (uint8_t const * p_data)
{
    return ((uint32_t) p_data[0] << 24U) |
           ((uint32_t) p_data[1] << 16U) |
           ((uint32_t) p_data[2] << 8U) |
           (uint32_t) p_data[3];
}

static void wire_protocol_write_u16_be (uint8_t * p_data, uint16_t value)
{
    p_data[0] = (uint8_t) (value >> 8U);
    p_data[1] = (uint8_t) value;
}

static void wire_protocol_write_u32_be (uint8_t * p_data, uint32_t value)
{
    p_data[0] = (uint8_t) (value >> 24U);
    p_data[1] = (uint8_t) (value >> 16U);
    p_data[2] = (uint8_t) (value >> 8U);
    p_data[3] = (uint8_t) value;
}

static app_status_t wire_protocol_record (wire_protocol_decoder_t * p_decoder,
                                          app_status_t status)
{
    p_decoder->diagnostics.last_status = status;
    return status;
}

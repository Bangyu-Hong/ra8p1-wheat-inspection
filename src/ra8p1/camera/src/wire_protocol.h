#ifndef WIRE_PROTOCOL_H_
#define WIRE_PROTOCOL_H_

#include <stdint.h>

#include "app_status.h"
#include "ra8p_wire_contract.h"

#define WIRE_PROTOCOL_MAX_FRAME_BYTES \
    (RA8P_PROTOCOL_HEADER_BYTES + RA8P_PROTOCOL_MAX_PAYLOAD_BYTES)

typedef struct st_wire_protocol_frame
{
    uint16_t        version;
    uint16_t        type;
    uint32_t        sequence;
    uint32_t        payload_bytes;
    uint32_t        payload_crc32;
    uint32_t        flags;
    uint8_t const * p_payload;
} wire_protocol_frame_t;

typedef struct st_wire_protocol_diagnostics
{
    uint32_t bytes_received;
    uint32_t frames_received;
    uint32_t crc_errors;
    uint32_t magic_errors;
    uint32_t version_errors;
    uint32_t oversized_frames;
    uint32_t buffer_overflows;
    uint32_t callback_busy;
    uint32_t callback_errors;
    uint32_t buffered_bytes;
    uint32_t buffer_high_water;
    app_status_t last_status;
} wire_protocol_diagnostics_t;

typedef struct st_wire_protocol_decoder
{
    uint8_t * p_storage;
    uint32_t  capacity;
    uint32_t  used;
    wire_protocol_diagnostics_t diagnostics;
} wire_protocol_decoder_t;

typedef app_status_t (*wire_protocol_frame_handler_t)(wire_protocol_frame_t const * p_frame,
                                                       void * p_context);

app_status_t wire_protocol_decoder_init(wire_protocol_decoder_t * p_decoder,
                                        uint8_t * p_storage,
                                        uint32_t capacity);
app_status_t wire_protocol_decoder_feed(wire_protocol_decoder_t * p_decoder,
                                        uint8_t const * p_data,
                                        uint32_t data_bytes,
                                        uint32_t * p_consumed_bytes,
                                        wire_protocol_frame_handler_t handler,
                                        void * p_context);
void wire_protocol_decoder_reset(wire_protocol_decoder_t * p_decoder);
void wire_protocol_decoder_get_diagnostics(wire_protocol_decoder_t const * p_decoder,
                                           wire_protocol_diagnostics_t * p_diagnostics);

app_status_t wire_protocol_encode(uint16_t type,
                                  uint32_t sequence,
                                  uint32_t flags,
                                  uint8_t const * p_payload,
                                  uint32_t payload_bytes,
                                  uint8_t * p_output,
                                  uint32_t output_capacity,
                                  uint32_t * p_output_bytes);
uint32_t wire_protocol_crc32(uint8_t const * p_data, uint32_t length);

#endif /* WIRE_PROTOCOL_H_ */

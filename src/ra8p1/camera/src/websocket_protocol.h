#ifndef WEBSOCKET_PROTOCOL_H_
#define WEBSOCKET_PROTOCOL_H_

#include <stdint.h>

#include "app_status.h"

#define WEBSOCKET_PROTOCOL_ACCEPT_KEY_BYTES  (29U)
#define WEBSOCKET_PROTOCOL_MAX_PAYLOAD_BYTES  (1024U)
#define WEBSOCKET_PROTOCOL_MAX_FRAME_BYTES    (1032U)

typedef enum e_websocket_protocol_opcode
{
    WEBSOCKET_PROTOCOL_OPCODE_TEXT = 0x1,
    WEBSOCKET_PROTOCOL_OPCODE_CLOSE = 0x8,
    WEBSOCKET_PROTOCOL_OPCODE_PING = 0x9,
    WEBSOCKET_PROTOCOL_OPCODE_PONG = 0xA
} websocket_protocol_opcode_t;

typedef struct st_websocket_protocol_frame
{
    websocket_protocol_opcode_t opcode;
    uint8_t const * p_payload;
    uint32_t payload_bytes;
} websocket_protocol_frame_t;

typedef app_status_t (* websocket_protocol_frame_handler_t)(
    websocket_protocol_frame_t const * p_frame,
    void * p_context);

typedef struct st_websocket_protocol_decoder
{
    uint8_t bytes[WEBSOCKET_PROTOCOL_MAX_FRAME_BYTES];
    uint32_t used;
} websocket_protocol_decoder_t;

app_status_t websocket_protocol_accept_key(char const * p_client_key,
                                           char p_accept_key[WEBSOCKET_PROTOCOL_ACCEPT_KEY_BYTES]);
void websocket_protocol_decoder_init(websocket_protocol_decoder_t * p_decoder);
app_status_t websocket_protocol_decoder_feed(websocket_protocol_decoder_t * p_decoder,
                                             uint8_t const * p_data,
                                             uint32_t data_bytes,
                                             websocket_protocol_frame_handler_t handler,
                                             void * p_context);
app_status_t websocket_protocol_encode(websocket_protocol_opcode_t opcode,
                                       uint8_t const * p_payload,
                                       uint32_t payload_bytes,
                                       uint8_t * p_output,
                                       uint32_t output_capacity,
                                       uint32_t * p_output_bytes);

#endif /* WEBSOCKET_PROTOCOL_H_ */

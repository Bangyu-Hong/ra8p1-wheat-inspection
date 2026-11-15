#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "http_protocol.h"
#include "websocket_protocol.h"

#define TEST_CHECK(condition)                                                        \
    do                                                                               \
    {                                                                                \
        if (!(condition))                                                            \
        {                                                                            \
            (void) fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                \
        }                                                                            \
    } while (0)

typedef struct st_frame_capture
{
    uint8_t payload[WEBSOCKET_PROTOCOL_MAX_PAYLOAD_BYTES];
    uint32_t payload_bytes;
    websocket_protocol_opcode_t opcode;
    uint32_t frames;
} frame_capture_t;

static app_status_t capture_frame(websocket_protocol_frame_t const * p_frame,
                                  void * p_context);
static uint32_t make_masked_frame(websocket_protocol_opcode_t opcode,
                                  uint8_t const * p_payload,
                                  uint32_t payload_bytes,
                                  uint8_t * p_output);
static int test_http_split_routes_and_limits(void);
static int test_websocket_handshake_vector(void);
static int test_websocket_frames(void);

static app_status_t capture_frame (websocket_protocol_frame_t const * p_frame,
                                   void * p_context)
{
    frame_capture_t * const p_capture = (frame_capture_t *) p_context;

    if ((NULL == p_frame) || (NULL == p_capture) ||
        (p_frame->payload_bytes > sizeof(p_capture->payload)))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (0U < p_frame->payload_bytes)
    {
        (void) memcpy(p_capture->payload, p_frame->p_payload, p_frame->payload_bytes);
    }
    p_capture->payload_bytes = p_frame->payload_bytes;
    p_capture->opcode = p_frame->opcode;
    p_capture->frames++;
    return APP_STATUS_OK;
}

static uint32_t make_masked_frame (websocket_protocol_opcode_t opcode,
                                   uint8_t const * p_payload,
                                   uint32_t payload_bytes,
                                   uint8_t * p_output)
{
    static uint8_t const mask[4] = {0x37U, 0xFAU, 0x21U, 0x3DU};
    uint32_t header_bytes = 6U;
    uint32_t index;

    p_output[0] = (uint8_t) (0x80U | (uint8_t) opcode);
    if (payload_bytes < 126U)
    {
        p_output[1] = (uint8_t) (0x80U | payload_bytes);
        (void) memcpy(&p_output[2], mask, sizeof(mask));
    }
    else
    {
        header_bytes = 8U;
        p_output[1] = 0xFEU;
        p_output[2] = (uint8_t) (payload_bytes >> 8U);
        p_output[3] = (uint8_t) payload_bytes;
        (void) memcpy(&p_output[4], mask, sizeof(mask));
    }
    for (index = 0U; index < payload_bytes; index++)
    {
        p_output[header_bytes + index] = p_payload[index] ^ mask[index & 3U];
    }
    return header_bytes + payload_bytes;
}

static int test_http_split_routes_and_limits (void)
{
    static char const request[] =
        "GET /ws HTTP/1.1\r\nHost: 192.168.1.8\r\nUpgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\nEXTRA";
    static char const health[] = "GET /health HTTP/1.0\r\n\r\n";
    static char const post[] = "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    static char const body[] = "GET / HTTP/1.1\r\nContent-Length: 1\r\n\r\nX";
    http_protocol_parser_t parser;
    http_protocol_request_t parsed;
    uint32_t consumed = 0U;
    uint8_t oversized[HTTP_PROTOCOL_MAX_HEADER_BYTES + 1U];

    http_protocol_parser_init(&parser);
    TEST_CHECK(APP_STATUS_BUSY == http_protocol_parser_feed(
        &parser, (uint8_t const *) request, 17U, &consumed, &parsed));
    TEST_CHECK(17U == consumed);
    TEST_CHECK(APP_STATUS_OK == http_protocol_parser_feed(
        &parser, (uint8_t const *) &request[17],
        (uint32_t) strlen(request) - 17U, &consumed, &parsed));
    TEST_CHECK(HTTP_PROTOCOL_ROUTE_WEBSOCKET == parsed.route);
    TEST_CHECK(parsed.websocket_upgrade);
    TEST_CHECK(0 == strcmp(parsed.websocket_key, "dGhlIHNhbXBsZSBub25jZQ=="));
    TEST_CHECK(consumed == ((uint32_t) strlen(request) - 17U - 5U));

    http_protocol_parser_init(&parser);
    TEST_CHECK(APP_STATUS_OK == http_protocol_parser_feed(
        &parser, (uint8_t const *) health, (uint32_t) strlen(health),
        &consumed, &parsed));
    TEST_CHECK(HTTP_PROTOCOL_ROUTE_HEALTH == parsed.route);
    TEST_CHECK(!parsed.websocket_upgrade);

    http_protocol_parser_init(&parser);
    TEST_CHECK(APP_STATUS_PROTOCOL == http_protocol_parser_feed(
        &parser, (uint8_t const *) post, (uint32_t) strlen(post),
        &consumed, &parsed));
    http_protocol_parser_init(&parser);
    TEST_CHECK(APP_STATUS_PROTOCOL == http_protocol_parser_feed(
        &parser, (uint8_t const *) body, (uint32_t) strlen(body),
        &consumed, &parsed));

    (void) memset(oversized, 'A', sizeof(oversized));
    http_protocol_parser_init(&parser);
    TEST_CHECK(APP_STATUS_DATA == http_protocol_parser_feed(
        &parser, oversized, sizeof(oversized), &consumed, &parsed));
    TEST_CHECK(HTTP_PROTOCOL_MAX_HEADER_BYTES == consumed);
    return 0;
}

static int test_websocket_handshake_vector (void)
{
    char accept[WEBSOCKET_PROTOCOL_ACCEPT_KEY_BYTES];

    TEST_CHECK(APP_STATUS_OK == websocket_protocol_accept_key(
        "dGhlIHNhbXBsZSBub25jZQ==", accept));
    TEST_CHECK(0 == strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    TEST_CHECK(APP_STATUS_PROTOCOL == websocket_protocol_accept_key("short", accept));
    return 0;
}

static int test_websocket_frames (void)
{
    uint8_t masked[WEBSOCKET_PROTOCOL_MAX_FRAME_BYTES];
    uint8_t output[WEBSOCKET_PROTOCOL_MAX_FRAME_BYTES];
    uint8_t payload[130];
    uint32_t masked_bytes;
    uint32_t output_bytes = 0U;
    uint32_t index;
    websocket_protocol_decoder_t decoder;
    frame_capture_t capture = {0};

    for (index = 0U; index < sizeof(payload); index++)
    {
        payload[index] = (uint8_t) index;
    }
    masked_bytes = make_masked_frame(WEBSOCKET_PROTOCOL_OPCODE_TEXT,
                                     payload, sizeof(payload), masked);
    websocket_protocol_decoder_init(&decoder);
    TEST_CHECK(APP_STATUS_BUSY == websocket_protocol_decoder_feed(
        &decoder, masked, 3U, capture_frame, &capture));
    TEST_CHECK(APP_STATUS_OK == websocket_protocol_decoder_feed(
        &decoder, &masked[3], masked_bytes - 3U, capture_frame, &capture));
    TEST_CHECK(1U == capture.frames);
    TEST_CHECK(WEBSOCKET_PROTOCOL_OPCODE_TEXT == capture.opcode);
    TEST_CHECK(sizeof(payload) == capture.payload_bytes);
    TEST_CHECK(0 == memcmp(payload, capture.payload, sizeof(payload)));

    TEST_CHECK(APP_STATUS_OK == websocket_protocol_encode(
        WEBSOCKET_PROTOCOL_OPCODE_PONG, payload, 7U, output, sizeof(output), &output_bytes));
    TEST_CHECK(9U == output_bytes);
    TEST_CHECK(0x8AU == output[0]);
    TEST_CHECK(7U == output[1]);
    TEST_CHECK(0 == memcmp(&output[2], payload, 7U));

    masked[0] = 0x01U;
    masked[1] = 0x80U;
    (void) memset(&masked[2], 0, 4U);
    websocket_protocol_decoder_init(&decoder);
    TEST_CHECK(APP_STATUS_PROTOCOL == websocket_protocol_decoder_feed(
        &decoder, masked, 6U, capture_frame, &capture));

    masked[0] = 0x81U;
    masked[1] = 0x00U;
    websocket_protocol_decoder_init(&decoder);
    TEST_CHECK(APP_STATUS_PROTOCOL == websocket_protocol_decoder_feed(
        &decoder, masked, 2U, capture_frame, &capture));
    return 0;
}

int main (void)
{
    TEST_CHECK(0 == test_http_split_routes_and_limits());
    TEST_CHECK(0 == test_websocket_handshake_vector());
    TEST_CHECK(0 == test_websocket_frames());
    (void) printf("http-websocket status=ok split=ok routes=3 header_limit=1024 "
                  "sha1=golden masking=required payload_limit=1024\n");
    return 0;
}

#include "websocket_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define WEBSOCKET_PROTOCOL_SHA1_BLOCK_BYTES  (64U)
#define WEBSOCKET_PROTOCOL_SHA1_DIGEST_BYTES (20U)

typedef struct st_websocket_sha1
{
    uint32_t state[5];
    uint64_t total_bytes;
    uint8_t block[WEBSOCKET_PROTOCOL_SHA1_BLOCK_BYTES];
    uint32_t block_used;
} websocket_sha1_t;

static void websocket_sha1_init(websocket_sha1_t * p_sha1);
static void websocket_sha1_update(websocket_sha1_t * p_sha1,
                                  uint8_t const * p_data,
                                  uint32_t data_bytes);
static void websocket_sha1_finish(websocket_sha1_t * p_sha1,
                                  uint8_t digest[WEBSOCKET_PROTOCOL_SHA1_DIGEST_BYTES]);
static void websocket_sha1_transform(websocket_sha1_t * p_sha1,
                                     uint8_t const block[WEBSOCKET_PROTOCOL_SHA1_BLOCK_BYTES]);
static uint32_t websocket_rotate_left(uint32_t value, uint32_t bits);
static void websocket_base64(uint8_t const * p_input,
                             uint32_t input_bytes,
                             char * p_output);
static bool websocket_opcode_valid(uint8_t opcode);

app_status_t websocket_protocol_accept_key (
    char const * p_client_key,
    char p_accept_key[WEBSOCKET_PROTOCOL_ACCEPT_KEY_BYTES])
{
    static char const websocket_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[WEBSOCKET_PROTOCOL_SHA1_DIGEST_BYTES];
    websocket_sha1_t sha1;
    size_t key_bytes;

    if ((NULL == p_client_key) || (NULL == p_accept_key))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    key_bytes = strlen(p_client_key);
    if (24U != key_bytes)
    {
        return APP_STATUS_PROTOCOL;
    }
    websocket_sha1_init(&sha1);
    websocket_sha1_update(&sha1, (uint8_t const *) p_client_key, (uint32_t) key_bytes);
    websocket_sha1_update(&sha1,
                          (uint8_t const *) websocket_guid,
                          (uint32_t) (sizeof(websocket_guid) - 1U));
    websocket_sha1_finish(&sha1, digest);
    websocket_base64(digest, sizeof(digest), p_accept_key);
    return APP_STATUS_OK;
}

void websocket_protocol_decoder_init (websocket_protocol_decoder_t * p_decoder)
{
    if (NULL != p_decoder)
    {
        *p_decoder = (websocket_protocol_decoder_t) {0};
    }
}

app_status_t websocket_protocol_decoder_feed (
    websocket_protocol_decoder_t * p_decoder,
    uint8_t const * p_data,
    uint32_t data_bytes,
    websocket_protocol_frame_handler_t handler,
    void * p_context)
{
    if ((NULL == p_decoder) || (NULL == handler) ||
        ((0U < data_bytes) && (NULL == p_data)))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (data_bytes > (WEBSOCKET_PROTOCOL_MAX_FRAME_BYTES - p_decoder->used))
    {
        return APP_STATUS_DATA;
    }
    if (0U < data_bytes)
    {
        (void) memcpy(&p_decoder->bytes[p_decoder->used], p_data, data_bytes);
        p_decoder->used += data_bytes;
    }

    while (p_decoder->used >= 2U)
    {
        uint8_t const first = p_decoder->bytes[0];
        uint8_t const second = p_decoder->bytes[1];
        uint8_t const opcode = (uint8_t) (first & 0x0FU);
        uint32_t header_bytes = 6U;
        uint32_t payload_bytes = (uint32_t) (second & 0x7FU);
        uint32_t frame_bytes;
        uint32_t index;
        websocket_protocol_frame_t frame;
        app_status_t status;

        if ((0U != (first & 0x70U)) || (0U == (first & 0x80U)) ||
            (0U == (second & 0x80U)) || !websocket_opcode_valid(opcode))
        {
            return APP_STATUS_PROTOCOL;
        }
        if (126U == payload_bytes)
        {
            if (p_decoder->used < 8U)
            {
                return APP_STATUS_BUSY;
            }
            payload_bytes = ((uint32_t) p_decoder->bytes[2] << 8U) |
                            p_decoder->bytes[3];
            header_bytes = 8U;
            if (payload_bytes < 126U)
            {
                return APP_STATUS_PROTOCOL;
            }
        }
        else if (127U == payload_bytes)
        {
            return APP_STATUS_DATA;
        }
        if ((payload_bytes > WEBSOCKET_PROTOCOL_MAX_PAYLOAD_BYTES) ||
            (((opcode & 0x08U) != 0U) && (payload_bytes > 125U)))
        {
            return APP_STATUS_DATA;
        }
        frame_bytes = header_bytes + payload_bytes;
        if (p_decoder->used < frame_bytes)
        {
            return APP_STATUS_BUSY;
        }
        for (index = 0U; index < payload_bytes; index++)
        {
            p_decoder->bytes[header_bytes + index] ^=
                p_decoder->bytes[header_bytes - 4U + (index & 3U)];
        }
        frame.opcode = (websocket_protocol_opcode_t) opcode;
        frame.p_payload = &p_decoder->bytes[header_bytes];
        frame.payload_bytes = payload_bytes;
        status = handler(&frame, p_context);
        if (APP_STATUS_OK != status)
        {
            for (index = 0U; index < payload_bytes; index++)
            {
                p_decoder->bytes[header_bytes + index] ^=
                    p_decoder->bytes[header_bytes - 4U + (index & 3U)];
            }
            return status;
        }
        p_decoder->used -= frame_bytes;
        if (0U < p_decoder->used)
        {
            (void) memmove(p_decoder->bytes,
                           &p_decoder->bytes[frame_bytes],
                           p_decoder->used);
        }
    }
    return (0U == p_decoder->used) ? APP_STATUS_OK : APP_STATUS_BUSY;
}

app_status_t websocket_protocol_encode (websocket_protocol_opcode_t opcode,
                                        uint8_t const * p_payload,
                                        uint32_t payload_bytes,
                                        uint8_t * p_output,
                                        uint32_t output_capacity,
                                        uint32_t * p_output_bytes)
{
    uint32_t header_bytes;

    if ((NULL == p_output) || (NULL == p_output_bytes) ||
        ((0U < payload_bytes) && (NULL == p_payload)) ||
        !websocket_opcode_valid((uint8_t) opcode) ||
        (payload_bytes > WEBSOCKET_PROTOCOL_MAX_PAYLOAD_BYTES) ||
        ((((uint8_t) opcode & 0x08U) != 0U) && (payload_bytes > 125U)))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    header_bytes = (payload_bytes < 126U) ? 2U : 4U;
    if (output_capacity < (header_bytes + payload_bytes))
    {
        return APP_STATUS_DATA;
    }
    p_output[0] = (uint8_t) (0x80U | (uint8_t) opcode);
    if (2U == header_bytes)
    {
        p_output[1] = (uint8_t) payload_bytes;
    }
    else
    {
        p_output[1] = 126U;
        p_output[2] = (uint8_t) (payload_bytes >> 8U);
        p_output[3] = (uint8_t) payload_bytes;
    }
    if (0U < payload_bytes)
    {
        (void) memcpy(&p_output[header_bytes], p_payload, payload_bytes);
    }
    *p_output_bytes = header_bytes + payload_bytes;
    return APP_STATUS_OK;
}

static void websocket_sha1_init (websocket_sha1_t * p_sha1)
{
    *p_sha1 = (websocket_sha1_t) {0};
    p_sha1->state[0] = 0x67452301UL;
    p_sha1->state[1] = 0xEFCDAB89UL;
    p_sha1->state[2] = 0x98BADCFEUL;
    p_sha1->state[3] = 0x10325476UL;
    p_sha1->state[4] = 0xC3D2E1F0UL;
}

static void websocket_sha1_update (websocket_sha1_t * p_sha1,
                                   uint8_t const * p_data,
                                   uint32_t data_bytes)
{
    uint32_t offset = 0U;

    p_sha1->total_bytes += data_bytes;
    while (offset < data_bytes)
    {
        uint32_t copy_bytes = WEBSOCKET_PROTOCOL_SHA1_BLOCK_BYTES - p_sha1->block_used;

        if (copy_bytes > (data_bytes - offset))
        {
            copy_bytes = data_bytes - offset;
        }
        (void) memcpy(&p_sha1->block[p_sha1->block_used], &p_data[offset], copy_bytes);
        p_sha1->block_used += copy_bytes;
        offset += copy_bytes;
        if (WEBSOCKET_PROTOCOL_SHA1_BLOCK_BYTES == p_sha1->block_used)
        {
            websocket_sha1_transform(p_sha1, p_sha1->block);
            p_sha1->block_used = 0U;
        }
    }
}

static void websocket_sha1_finish (
    websocket_sha1_t * p_sha1,
    uint8_t digest[WEBSOCKET_PROTOCOL_SHA1_DIGEST_BYTES])
{
    uint64_t const total_bits = p_sha1->total_bytes * 8U;
    uint8_t one = 0x80U;
    uint8_t zero = 0U;
    uint8_t length[8];
    uint32_t index;

    for (index = 0U; index < sizeof(length); index++)
    {
        length[7U - index] = (uint8_t) (total_bits >> (index * 8U));
    }
    websocket_sha1_update(p_sha1, &one, 1U);
    while (56U != p_sha1->block_used)
    {
        websocket_sha1_update(p_sha1, &zero, 1U);
    }
    websocket_sha1_update(p_sha1, length, sizeof(length));
    for (index = 0U; index < 5U; index++)
    {
        digest[index * 4U] = (uint8_t) (p_sha1->state[index] >> 24U);
        digest[index * 4U + 1U] = (uint8_t) (p_sha1->state[index] >> 16U);
        digest[index * 4U + 2U] = (uint8_t) (p_sha1->state[index] >> 8U);
        digest[index * 4U + 3U] = (uint8_t) p_sha1->state[index];
    }
}

static void websocket_sha1_transform (
    websocket_sha1_t * p_sha1,
    uint8_t const block[WEBSOCKET_PROTOCOL_SHA1_BLOCK_BYTES])
{
    uint32_t words[80];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t index;

    for (index = 0U; index < 16U; index++)
    {
        words[index] = ((uint32_t) block[index * 4U] << 24U) |
                       ((uint32_t) block[index * 4U + 1U] << 16U) |
                       ((uint32_t) block[index * 4U + 2U] << 8U) |
                       block[index * 4U + 3U];
    }
    for (index = 16U; index < 80U; index++)
    {
        words[index] = websocket_rotate_left(words[index - 3U] ^ words[index - 8U] ^
                                             words[index - 14U] ^ words[index - 16U], 1U);
    }
    a = p_sha1->state[0];
    b = p_sha1->state[1];
    c = p_sha1->state[2];
    d = p_sha1->state[3];
    e = p_sha1->state[4];
    for (index = 0U; index < 80U; index++)
    {
        uint32_t function;
        uint32_t constant;
        uint32_t temporary;

        if (index < 20U)
        {
            function = (b & c) | ((~b) & d);
            constant = 0x5A827999UL;
        }
        else if (index < 40U)
        {
            function = b ^ c ^ d;
            constant = 0x6ED9EBA1UL;
        }
        else if (index < 60U)
        {
            function = (b & c) | (b & d) | (c & d);
            constant = 0x8F1BBCDCUL;
        }
        else
        {
            function = b ^ c ^ d;
            constant = 0xCA62C1D6UL;
        }
        temporary = websocket_rotate_left(a, 5U) + function + e + constant + words[index];
        e = d;
        d = c;
        c = websocket_rotate_left(b, 30U);
        b = a;
        a = temporary;
    }
    p_sha1->state[0] += a;
    p_sha1->state[1] += b;
    p_sha1->state[2] += c;
    p_sha1->state[3] += d;
    p_sha1->state[4] += e;
}

static uint32_t websocket_rotate_left (uint32_t value, uint32_t bits)
{
    return (value << bits) | (value >> (32U - bits));
}

static void websocket_base64 (uint8_t const * p_input,
                              uint32_t input_bytes,
                              char * p_output)
{
    static char const alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t input_index = 0U;
    uint32_t output_index = 0U;

    while (input_index < input_bytes)
    {
        uint32_t const remaining = input_bytes - input_index;
        uint32_t const first = p_input[input_index++];
        uint32_t const second = (remaining > 1U) ? p_input[input_index++] : 0U;
        uint32_t const third = (remaining > 2U) ? p_input[input_index++] : 0U;
        uint32_t const value = (first << 16U) | (second << 8U) | third;

        p_output[output_index++] = alphabet[(value >> 18U) & 0x3FU];
        p_output[output_index++] = alphabet[(value >> 12U) & 0x3FU];
        p_output[output_index++] = (remaining > 1U) ? alphabet[(value >> 6U) & 0x3FU] : '=';
        p_output[output_index++] = (remaining > 2U) ? alphabet[value & 0x3FU] : '=';
    }
    p_output[output_index] = '\0';
}

static bool websocket_opcode_valid (uint8_t opcode)
{
    return (WEBSOCKET_PROTOCOL_OPCODE_TEXT == opcode) ||
           (WEBSOCKET_PROTOCOL_OPCODE_CLOSE == opcode) ||
           (WEBSOCKET_PROTOCOL_OPCODE_PING == opcode) ||
           (WEBSOCKET_PROTOCOL_OPCODE_PONG == opcode);
}

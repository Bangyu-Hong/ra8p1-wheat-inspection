#include "esp8266_link.h"

#include "app_config.h"
#include "board_port.h"

#include <stdio.h>
#include <string.h>

#define FRAME_SOF_0       0x55U
#define FRAME_SOF_1       0xAAU
#define FRAME_VERSION     0x01U
#define FRAME_TYPE_HELLO  0x01U
#define FRAME_TYPE_RECORD 0x10U
#define FRAME_TYPE_ACK    0x80U

static uint16_t g_sequence;
static bool g_online;

static uint16_t crc16_ccitt(const uint8_t * data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    size_t index;
    uint8_t bit;
    for (index = 0U; index < length; ++index)
    {
        crc ^= (uint16_t) data[index] << 8U;
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc & 0x8000U) ? (uint16_t) ((crc << 1U) ^ 0x1021U) : (uint16_t) (crc << 1U);
        }
    }
    return crc;
}

static app_status_t wait_for_ack(uint16_t expected_sequence)
{
    uint8_t receive[64];
    size_t used = 0U;
    uint32_t start_ms = board_time_ms();

    while ((uint32_t) (board_time_ms() - start_ms) < ESP_ACK_TIMEOUT_MS)
    {
        used += board_esp_uart_read(receive + used, sizeof(receive) - used);
        if (used >= 11U)
        {
            size_t offset;
            for (offset = 0U; offset + 11U <= used; ++offset)
            {
                uint16_t sequence;
                uint16_t payload_length;
                uint16_t received_crc;
                uint16_t calculated_crc;
                if ((receive[offset] != FRAME_SOF_0) || (receive[offset + 1U] != FRAME_SOF_1))
                {
                    continue;
                }
                sequence = (uint16_t) receive[offset + 4U] |
                           ((uint16_t) receive[offset + 5U] << 8U);
                payload_length = (uint16_t) receive[offset + 6U] |
                                 ((uint16_t) receive[offset + 7U] << 8U);
                if ((receive[offset + 2U] != FRAME_VERSION) ||
                    (receive[offset + 3U] != FRAME_TYPE_ACK) ||
                    (payload_length != 1U) || (sequence != expected_sequence))
                {
                    continue;
                }
                received_crc = (uint16_t) receive[offset + 9U] |
                               ((uint16_t) receive[offset + 10U] << 8U);
                calculated_crc = crc16_ccitt(receive + offset + 2U, 7U);
                if ((received_crc == calculated_crc) && (receive[offset + 8U] == 0U))
                {
                    return APP_OK;
                }
                return APP_ERR_PROTOCOL;
            }
        }
        if (used == sizeof(receive))
        {
            used = 0U;
        }
    }
    return APP_ERR_TIMEOUT;
}

static app_status_t send_frame(uint8_t type, const uint8_t * payload, uint16_t payload_length)
{
    uint8_t frame[ESP_FRAME_MAX_PAYLOAD + 10U];
    uint16_t sequence = ++g_sequence;
    uint16_t crc;
    app_status_t status;

    if ((payload_length > ESP_FRAME_MAX_PAYLOAD) || ((payload == NULL) && (payload_length > 0U)))
    {
        return APP_ERR_ARGUMENT;
    }

    frame[0] = FRAME_SOF_0;
    frame[1] = FRAME_SOF_1;
    frame[2] = FRAME_VERSION;
    frame[3] = type;
    frame[4] = (uint8_t) sequence;
    frame[5] = (uint8_t) (sequence >> 8U);
    frame[6] = (uint8_t) payload_length;
    frame[7] = (uint8_t) (payload_length >> 8U);
    if (payload_length > 0U)
    {
        memcpy(frame + 8U, payload, payload_length);
    }
    crc = crc16_ccitt(frame + 2U, (size_t) payload_length + 6U);
    frame[8U + payload_length] = (uint8_t) crc;
    frame[9U + payload_length] = (uint8_t) (crc >> 8U);

    status = board_esp_uart_write(frame, (size_t) payload_length + 10U);
    if (status != APP_OK)
    {
        return status;
    }
    return wait_for_ack(sequence);
}

app_status_t esp8266_link_init(void)
{
    app_status_t status = send_frame(FRAME_TYPE_HELLO, NULL, 0U);
    g_online = (status == APP_OK);
    return status;
}

bool esp8266_link_is_online(void)
{
    return g_online;
}

app_status_t esp8266_send_record(const detection_record_t * record)
{
    char json[ESP_FRAME_MAX_PAYLOAD];
    int length;
    app_status_t status;

    if (record == NULL)
    {
        return APP_ERR_ARGUMENT;
    }
    length = snprintf(json,
                      sizeof(json),
                      "{\"sample\":\"%s\",\"time\":%lu,\"count\":%u,"
                      "\"temp\":%d,\"humidity\":%u,\"env_valid\":%u,"
                      "\"light\":%u,\"quality\":%u}",
                      record->sample_id,
                      (unsigned long) record->timestamp_s,
                      record->wheat_ear_count,
                      record->environment.temperature_c,
                      record->environment.humidity_percent,
                      record->environment.valid ? 1U : 0U,
                      record->light_percent,
                      (unsigned int) record->image_quality.code);
    if ((length < 0) || ((size_t) length >= sizeof(json)))
    {
        return APP_ERR_NO_SPACE;
    }

    status = send_frame(FRAME_TYPE_RECORD, (const uint8_t *) json, (uint16_t) length);
    if (status != APP_OK)
    {
        g_online = false;
    }
    return status;
}

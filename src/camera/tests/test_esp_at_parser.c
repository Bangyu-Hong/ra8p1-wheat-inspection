#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_at_parser.h"

#define TEST_CHECK(condition)                                                       \
    do                                                                              \
    {                                                                               \
        if (!(condition))                                                           \
        {                                                                           \
            (void) printf("check failed: %s:%d: %s\n", __FILE__, __LINE__,       \
                          #condition);                                               \
            return 1;                                                               \
        }                                                                           \
    } while (0)

typedef struct st_parser_sink
{
    uint32_t line_calls;
    uint32_t ipd_calls;
    uint32_t payload_bytes;
    uint8_t last_link_id;
    char lines[128U];
    uint32_t lines_used;
    uint8_t payload[128U];
    bool fail_next_ipd;
} parser_sink_t;

static app_status_t capture_line(char const * p_line, uint32_t line_bytes, void * p_context);
static app_status_t capture_ipd(uint8_t link_id,
                                uint8_t const * p_data,
                                uint32_t data_bytes,
                                void * p_context);
static int test_lines_prompt_and_partial_ipd(void);
static int test_single_link_and_remote_ip_forms(void);
static int test_sticky_ipd_and_lines(void);
static int test_malformed_and_overflow_recovery(void);
static int test_ipd_bounds_and_callback_ownership(void);

int main (void)
{
    TEST_CHECK(0 == test_lines_prompt_and_partial_ipd());
    TEST_CHECK(0 == test_single_link_and_remote_ip_forms());
    TEST_CHECK(0 == test_sticky_ipd_and_lines());
    TEST_CHECK(0 == test_malformed_and_overflow_recovery());
    TEST_CHECK(0 == test_ipd_bounds_and_callback_ownership());

    (void) printf("esp-at-parser status=ok partial=ok sticky=ok ipd4=ok "
                  "overflow=resync callback=consumed\n");
    return 0;
}

static app_status_t capture_line (char const * p_line,
                                  uint32_t line_bytes,
                                  void * p_context)
{
    parser_sink_t * const p_sink = (parser_sink_t *) p_context;

    if ((NULL == p_line) || (NULL == p_sink) ||
        (line_bytes > ((uint32_t) sizeof(p_sink->lines) - p_sink->lines_used - 1U)))
    {
        return APP_STATUS_DATA;
    }
    (void) memcpy(&p_sink->lines[p_sink->lines_used], p_line, line_bytes);
    p_sink->lines_used += line_bytes;
    p_sink->lines[p_sink->lines_used++] = '|';
    p_sink->lines[p_sink->lines_used] = '\0';
    p_sink->line_calls++;
    return APP_STATUS_OK;
}

static app_status_t capture_ipd (uint8_t link_id,
                                 uint8_t const * p_data,
                                 uint32_t data_bytes,
                                 void * p_context)
{
    parser_sink_t * const p_sink = (parser_sink_t *) p_context;

    if ((NULL == p_data) || (NULL == p_sink) ||
        (data_bytes > ((uint32_t) sizeof(p_sink->payload) - p_sink->payload_bytes)))
    {
        return APP_STATUS_DATA;
    }
    (void) memcpy(&p_sink->payload[p_sink->payload_bytes], p_data, data_bytes);
    p_sink->payload_bytes += data_bytes;
    p_sink->last_link_id = link_id;
    p_sink->ipd_calls++;
    if (p_sink->fail_next_ipd)
    {
        p_sink->fail_next_ipd = false;
        return APP_STATUS_BUSY;
    }
    return APP_STATUS_OK;
}

static int test_lines_prompt_and_partial_ipd (void)
{
    static uint8_t const first[] = "\r\nOK\r\n> +IPD,2,5:he";
    static uint8_t const second[] = "llo\r\nWIFI CONNECTED\r\n";
    esp_at_parser_t parser;
    esp_at_parser_diagnostics_t diagnostics;
    parser_sink_t sink = {0};

    esp_at_parser_init(&parser);
    TEST_CHECK(APP_STATUS_OK == esp_at_parser_feed(
        &parser, first, (uint32_t) (sizeof(first) - 1U), capture_line, capture_ipd, &sink));
    TEST_CHECK(2U == sink.payload_bytes);
    TEST_CHECK(1U == sink.ipd_calls);
    TEST_CHECK(3U == parser.ipd_remaining);
    TEST_CHECK(APP_STATUS_OK == esp_at_parser_feed(
        &parser, second, (uint32_t) (sizeof(second) - 1U), capture_line, capture_ipd, &sink));
    TEST_CHECK(0 == strcmp(sink.lines, "OK|>|WIFI CONNECTED|"));
    TEST_CHECK(0 == memcmp(sink.payload, "hello", 5U));
    TEST_CHECK(2U == sink.last_link_id);

    esp_at_parser_get_diagnostics(&parser, &diagnostics);
    TEST_CHECK(((sizeof(first) - 1U) + (sizeof(second) - 1U)) == diagnostics.bytes_received);
    TEST_CHECK(2U == diagnostics.lines_received);
    TEST_CHECK(1U == diagnostics.prompts_received);
    TEST_CHECK(1U == diagnostics.ipd_frames);
    TEST_CHECK(5U == diagnostics.ipd_bytes);
    TEST_CHECK(0U == diagnostics.ipd_bytes_remaining);
    return 0;
}

static int test_single_link_and_remote_ip_forms (void)
{
    static uint8_t const input[] =
        "+IPD,3:one+IPD,4,3:two+IPD,5,192.168.1.2,8266:three"
        "+IPD,1,4,10.0.0.2,9000:four";
    esp_at_parser_t parser;
    esp_at_parser_diagnostics_t diagnostics;
    parser_sink_t sink = {0};

    esp_at_parser_init(&parser);
    TEST_CHECK(APP_STATUS_OK == esp_at_parser_feed(
        &parser, input, (uint32_t) (sizeof(input) - 1U), capture_line, capture_ipd, &sink));
    TEST_CHECK(0 == memcmp(sink.payload, "onetwothreefour", 15U));
    TEST_CHECK(1U == sink.last_link_id);
    esp_at_parser_get_diagnostics(&parser, &diagnostics);
    TEST_CHECK(4U == diagnostics.ipd_frames);
    TEST_CHECK(15U == diagnostics.ipd_bytes);
    return 0;
}

static int test_sticky_ipd_and_lines (void)
{
    static uint8_t const input[] = "+IPD,0,5:AB\r\n\0\r\n0,CLOSED\r\nOK\r\n";
    esp_at_parser_t parser;
    parser_sink_t sink = {0};

    esp_at_parser_init(&parser);
    TEST_CHECK(APP_STATUS_OK == esp_at_parser_feed(
        &parser, input, (uint32_t) (sizeof(input) - 1U), capture_line, capture_ipd, &sink));
    TEST_CHECK(5U == sink.payload_bytes);
    TEST_CHECK(0 == memcmp(sink.payload, "AB\r\n\0", 5U));
    TEST_CHECK(0 == strcmp(sink.lines, "0,CLOSED|OK|"));
    return 0;
}

static int test_malformed_and_overflow_recovery (void)
{
    uint8_t input[ESP_AT_PARSER_LINE_BYTES + 16U];
    static uint8_t const recovered[] = "\r\nOK\r\n";
    static uint8_t const malformed[] = "+IPD,9,2:xx\r\nready\r\n";
    esp_at_parser_t parser;
    esp_at_parser_diagnostics_t diagnostics;
    parser_sink_t sink = {0};

    (void) memset(input, 'X', sizeof(input));
    input[sizeof(input) - 1U] = '\n';
    esp_at_parser_init(&parser);
    TEST_CHECK(APP_STATUS_PROTOCOL == esp_at_parser_feed(
        &parser, input, (uint32_t) sizeof(input), capture_line, capture_ipd, &sink));
    TEST_CHECK(APP_STATUS_OK == esp_at_parser_feed(
        &parser, recovered, (uint32_t) (sizeof(recovered) - 1U), capture_line, capture_ipd, &sink));
    TEST_CHECK(0 == strcmp(sink.lines, "OK|"));

    TEST_CHECK(APP_STATUS_PROTOCOL == esp_at_parser_feed(
        &parser, malformed, (uint32_t) (sizeof(malformed) - 1U), capture_line, capture_ipd, &sink));
    TEST_CHECK(0 == strcmp(sink.lines, "OK|ready|"));
    esp_at_parser_get_diagnostics(&parser, &diagnostics);
    TEST_CHECK(1U == diagnostics.line_overflows);
    TEST_CHECK(1U == diagnostics.malformed_ipd);
    TEST_CHECK(0U == diagnostics.buffered_line_bytes);
    TEST_CHECK(0U < diagnostics.discarded_bytes);
    return 0;
}

static int test_ipd_bounds_and_callback_ownership (void)
{
    static uint8_t oversized[ESP_AT_PARSER_MAX_IPD_BYTES + 32U];
    static char const oversized_header[] = "+IPD,8193:";
    static char const oversized_tail[] = "\r\nOK\r\n";
    static uint8_t const callback_failure[] = "+IPD,4:dataOK\r\n";
    uint32_t oversized_bytes;
    esp_at_parser_t parser;
    esp_at_parser_diagnostics_t diagnostics;
    parser_sink_t sink = {0};

    (void) memcpy(oversized, oversized_header, sizeof(oversized_header) - 1U);
    (void) memset(&oversized[sizeof(oversized_header) - 1U],
                  '\n',
                  ESP_AT_PARSER_MAX_IPD_BYTES + 1U);
    oversized_bytes = (uint32_t) (sizeof(oversized_header) - 1U) +
                      ESP_AT_PARSER_MAX_IPD_BYTES + 1U;
    (void) memcpy(&oversized[oversized_bytes],
                  oversized_tail,
                  sizeof(oversized_tail) - 1U);
    oversized_bytes += (uint32_t) (sizeof(oversized_tail) - 1U);

    esp_at_parser_init(&parser);
    TEST_CHECK(APP_STATUS_PROTOCOL == esp_at_parser_feed(
        &parser, oversized, oversized_bytes, capture_line, capture_ipd, &sink));
    TEST_CHECK(0 == strcmp(sink.lines, "OK|"));
    esp_at_parser_get_diagnostics(&parser, &diagnostics);
    TEST_CHECK(1U == diagnostics.oversized_ipd);

    esp_at_parser_init(&parser);
    sink = (parser_sink_t) {0};
    sink.fail_next_ipd = true;
    TEST_CHECK(APP_STATUS_BUSY == esp_at_parser_feed(
        &parser,
        callback_failure,
        (uint32_t) (sizeof(callback_failure) - 1U),
        capture_line,
        capture_ipd,
        &sink));
    TEST_CHECK(4U == sink.payload_bytes);
    TEST_CHECK(0U == parser.ipd_remaining);
    TEST_CHECK(APP_STATUS_OK == esp_at_parser_feed(
        &parser, &callback_failure[11U], 4U, capture_line, capture_ipd, &sink));
    TEST_CHECK(0 == strcmp(sink.lines, "OK|"));
    esp_at_parser_get_diagnostics(&parser, &diagnostics);
    TEST_CHECK(1U == diagnostics.callback_errors);
    TEST_CHECK(4U == diagnostics.ipd_bytes);
    return 0;
}

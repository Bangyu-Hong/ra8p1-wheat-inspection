#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_telemetry.h"
#include "local_tcp_session.h"
#include "ra8p_wire_contract.h"
#include "wire_protocol.h"

#define TEST_CHECK(condition)                                                        \
    do                                                                               \
    {                                                                                \
        if (!(condition))                                                            \
        {                                                                            \
            (void) fprintf(stderr, "test failure line=%d: %s\n", __LINE__, #condition); \
            return 1;                                                                \
        }                                                                            \
    } while (0)

static uint16_t read_u16_be(uint8_t const * p_data);
static uint32_t read_u32_be(uint8_t const * p_data);
static bool payload_contains(local_tcp_outbound_t const * p_outbound,
                             char const * p_expected);
static int test_handshake_ping_and_fragmentation(void);
static int test_single_link_and_queue_priorities(void);
static int test_in_flight_buffer_ownership(void);
static int test_offline_cache_and_reconnect_snapshot(void);
static int test_realistic_embedded_snapshot_values(void);
static int test_hello_ack_survives_snapshot_format_error(void);

static uint16_t read_u16_be (uint8_t const * p_data)
{
    return (uint16_t) (((uint16_t) p_data[0] << 8U) | (uint16_t) p_data[1]);
}

static uint32_t read_u32_be (uint8_t const * p_data)
{
    return ((uint32_t) p_data[0] << 24U) |
           ((uint32_t) p_data[1] << 16U) |
           ((uint32_t) p_data[2] << 8U) |
           (uint32_t) p_data[3];
}

static bool payload_contains (local_tcp_outbound_t const * p_outbound,
                              char const * p_expected)
{
    uint32_t payload_bytes;
    size_t expected_bytes;
    uint32_t offset;

    if ((NULL == p_outbound) || (NULL == p_outbound->p_data) ||
        (NULL == p_expected) ||
        (p_outbound->data_bytes < RA8P_PROTOCOL_HEADER_BYTES))
    {
        return false;
    }
    payload_bytes = read_u32_be(
        &p_outbound->p_data[RA8P_HEADER_PAYLOAD_BYTES_OFFSET]);
    expected_bytes = strlen(p_expected);
    if ((payload_bytes > (p_outbound->data_bytes - RA8P_PROTOCOL_HEADER_BYTES)) ||
        (expected_bytes > payload_bytes))
    {
        return false;
    }
    for (offset = 0U;
         offset <= (payload_bytes - (uint32_t) expected_bytes);
         offset++)
    {
        if (0 == memcmp(&p_outbound->p_data[RA8P_PROTOCOL_HEADER_BYTES + offset],
                        p_expected,
                        expected_bytes))
        {
            return true;
        }
    }
    return false;
}

static int test_handshake_ping_and_fragmentation (void)
{
    uint8_t hello[128];
    uint8_t ping[128];
    uint8_t sticky[256];
    uint32_t hello_bytes = 0U;
    uint32_t ping_bytes = 0U;
    local_tcp_outbound_t outbound;
    local_tcp_session_diagnostics_t diagnostics;

    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_HELLO,
        7U,
        RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{\"client\":\"test\"}",
        17U,
        hello,
        sizeof(hello),
        &hello_bytes));
    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_PING,
        8U,
        RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{\"monotonic\":1}",
        15U,
        ping,
        sizeof(ping),
        &ping_bytes));
    (void) memcpy(sticky, hello, hello_bytes);
    (void) memcpy(&sticky[hello_bytes], ping, ping_bytes);

    local_tcp_session_init();
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_open_link(0U));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(0U, sticky, 11U));
    TEST_CHECK(APP_STATUS_UNAVAILABLE == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(
        0U, &sticky[11U], hello_bytes + ping_bytes - 11U));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(outbound.control_response);
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    TEST_CHECK(RA8P_MESSAGE_HELLO_ACK == read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(7U == read_u32_be(&outbound.p_data[RA8P_HEADER_SEQUENCE_OFFSET]));
    local_tcp_session_consume_outbound(true, true);

    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(outbound.control_response);
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    TEST_CHECK(RA8P_MESSAGE_PONG == read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(8U == read_u32_be(&outbound.p_data[RA8P_HEADER_SEQUENCE_OFFSET]));
    local_tcp_session_consume_outbound(true, true);

    local_tcp_session_get_diagnostics(&diagnostics);
    TEST_CHECK(diagnostics.negotiated);
    TEST_CHECK(2U == diagnostics.frames_received);
    TEST_CHECK(1U == diagnostics.hello_requests);
    TEST_CHECK(1U == diagnostics.ping_requests);
    TEST_CHECK(2U == diagnostics.responses_queued);
    TEST_CHECK(0U == diagnostics.response_queue_busy);
    TEST_CHECK(2U == diagnostics.response_queue_high_water);
    TEST_CHECK(0U == diagnostics.protocol_errors);
    TEST_CHECK(APP_STATUS_UNAVAILABLE == local_tcp_session_peek_outbound(&outbound));

    return 0;
}

static int test_single_link_and_queue_priorities (void)
{
    uint8_t hello[128];
    uint8_t command[128];
    uint32_t hello_bytes = 0U;
    uint32_t command_bytes = 0U;
    app_telemetry_snapshot_t snapshot = {0};
    local_tcp_outbound_t outbound;
    local_tcp_session_diagnostics_t diagnostics;

    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_HELLO,
        20U,
        RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{}",
        2U,
        hello,
        sizeof(hello),
        &hello_bytes));
    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_COMMAND,
        21U,
        RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{\"action\":\"start\"}",
        18U,
        command,
        sizeof(command),
        &command_bytes));

    local_tcp_session_init();
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_open_link(2U));
    TEST_CHECK(APP_STATUS_BUSY == local_tcp_session_open_link(3U));
    TEST_CHECK(APP_STATUS_UNAVAILABLE == local_tcp_session_feed(3U, hello, hello_bytes));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(2U, hello, hello_bytes));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    local_tcp_session_consume_outbound(true, true);

    snapshot.schema_version = APP_TELEMETRY_SCHEMA_VERSION;
    snapshot.sequence = 100U;
    snapshot.capabilities = APP_TELEMETRY_FOUNDATION_CAPABILITIES |
                            RA8P_CAPABILITY_ENVIRONMENT |
                            RA8P_CAPABILITY_LAN_TCP;
    snapshot.temperature_milli_c = -1250;
    snapshot.humidity_milli_percent = 43250U;
    snapshot.light_raw = 2400U;
    snapshot.light_average = 2350U;
    snapshot.light_tenths_percent = 574U;
    snapshot.wheat_count = 37;
    snapshot.mission_wheat_count = 137U;
    snapshot.field_wheat_count = 1137U;
    snapshot.counted_frame_count = 11U;
    snapshot.slide_position_index = 3U;
    snapshot.region_status = APP_REGION_STATUS_COMPLETED;
    snapshot.inference_us = 184250U;
    (void) strcpy(snapshot.device_id, "ra8p1-test");
    (void) strcpy(snapshot.session_id, "lan-2");
    (void) strcpy(snapshot.build_version, "test");
    (void) strcpy(snapshot.model_version, "yolox-test");

    TEST_CHECK(APP_STATUS_OK == local_tcp_session_queue_telemetry(&snapshot));
    snapshot.sequence = 101U;
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_queue_telemetry(&snapshot));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(2U, command, command_bytes));

    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(outbound.control_response);
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    TEST_CHECK(RA8P_MESSAGE_COMMAND_ACK == read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    local_tcp_session_consume_outbound(true, true);
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(!outbound.control_response);
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(false));
    TEST_CHECK(RA8P_MESSAGE_STATE_SNAPSHOT ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(RA8P_FLAG_SNAPSHOT ==
               read_u32_be(&outbound.p_data[RA8P_HEADER_FLAGS_OFFSET]));
    TEST_CHECK(101U == read_u32_be(&outbound.p_data[RA8P_HEADER_SEQUENCE_OFFSET]));
    TEST_CHECK(payload_contains(&outbound, "\"temperature_c\":-1.250"));
    TEST_CHECK(payload_contains(&outbound, "\"mission_wheat_count\":137"));
    TEST_CHECK(payload_contains(&outbound, "\"field_wheat_count\":1137"));
    TEST_CHECK(payload_contains(&outbound, "\"counted_frame_count\":11"));
    TEST_CHECK(payload_contains(&outbound, "\"slide_position_index\":3"));
    TEST_CHECK(payload_contains(&outbound, "\"region_status\":5"));
    local_tcp_session_consume_outbound(false, true);

    snapshot.sequence = 102U;
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_queue_telemetry(&snapshot));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(!outbound.control_response);
    TEST_CHECK(RA8P_MESSAGE_TELEMETRY ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(RA8P_FLAG_NONE ==
               read_u32_be(&outbound.p_data[RA8P_HEADER_FLAGS_OFFSET]));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(false));
    local_tcp_session_consume_outbound(false, true);

    local_tcp_session_close_link(3U);
    local_tcp_session_get_diagnostics(&diagnostics);
    TEST_CHECK(3U == diagnostics.telemetry_queued);
    TEST_CHECK(1U == diagnostics.telemetry_replaced);
    TEST_CHECK(2U == diagnostics.snapshots_queued);
    TEST_CHECK(1U == diagnostics.snapshots_sent);
    TEST_CHECK(!diagnostics.snapshot_pending);
    TEST_CHECK(1U == diagnostics.links_rejected);
    TEST_CHECK(0U == diagnostics.links_closed);
    local_tcp_session_close_link(2U);
    local_tcp_session_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.links_closed);
    TEST_CHECK(LOCAL_TCP_SESSION_NO_LINK == diagnostics.active_link_id);

    return 0;
}

static int test_in_flight_buffer_ownership (void)
{
    uint8_t hello[128];
    uint8_t command[128];
    uint32_t hello_bytes = 0U;
    uint32_t command_bytes = 0U;
    app_telemetry_snapshot_t snapshot = {0};
    local_tcp_outbound_t outbound;
    local_tcp_session_diagnostics_t diagnostics;

    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_HELLO, 30U, RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{}", 2U, hello, sizeof(hello), &hello_bytes));
    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_COMMAND, 31U, RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{}", 2U, command, sizeof(command), &command_bytes));

    local_tcp_session_init();
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_open_link(1U));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(1U, hello, hello_bytes));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    local_tcp_session_consume_outbound(true, true);

    snapshot.schema_version = APP_TELEMETRY_SCHEMA_VERSION;
    snapshot.sequence = 200U;
    snapshot.wheat_count = APP_TELEMETRY_WHEAT_COUNT_UNKNOWN;
    (void) strcpy(snapshot.device_id, "ra8p1-test");
    (void) strcpy(snapshot.session_id, "lan-1");
    (void) strcpy(snapshot.build_version, "test");
    (void) strcpy(snapshot.model_version, "test");
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_queue_telemetry(&snapshot));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(!outbound.control_response);
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(false));
    snapshot.sequence++;
    TEST_CHECK(APP_STATUS_BUSY == local_tcp_session_queue_telemetry(&snapshot));

    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(1U, command, command_bytes));
    local_tcp_session_consume_outbound(false, true);
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(outbound.control_response);
    TEST_CHECK(RA8P_MESSAGE_COMMAND_ACK ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    local_tcp_session_consume_outbound(true, true);

    local_tcp_session_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.telemetry_queue_busy);
    TEST_CHECK(0U == diagnostics.telemetry_replaced);
    return 0;
}

static int test_offline_cache_and_reconnect_snapshot (void)
{
    uint8_t hello[128];
    uint32_t hello_bytes = 0U;
    app_telemetry_snapshot_t snapshot = {0};
    local_tcp_outbound_t outbound;
    local_tcp_session_diagnostics_t diagnostics;

    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_HELLO, 40U, RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{}", 2U, hello, sizeof(hello), &hello_bytes));

    snapshot.schema_version = APP_TELEMETRY_SCHEMA_VERSION;
    snapshot.sequence = 300U;
    snapshot.capabilities = APP_TELEMETRY_FOUNDATION_CAPABILITIES |
                            RA8P_CAPABILITY_ENVIRONMENT |
                            RA8P_CAPABILITY_LAN_TCP;
    snapshot.environment_valid = true;
    snapshot.light_valid = true;
    snapshot.temperature_milli_c = 24625;
    snapshot.humidity_milli_percent = 61250U;
    snapshot.light_raw = 2300U;
    snapshot.light_average = 2280U;
    snapshot.light_tenths_percent = 557U;
    snapshot.monotonic_us = 123456789U;
    snapshot.wall_time_ms = 1786900000123ULL;
    snapshot.wall_time_valid = true;
    snapshot.target_position_mm = 1200U;
    snapshot.actual_position_mm = -25;
    (void) strcpy(snapshot.device_id, "ra8p1-cache-test");
    (void) strcpy(snapshot.session_id, "boot-cache-test");
    (void) strcpy(snapshot.build_version, "offline-build");
    (void) strcpy(snapshot.model_version, "yolox-cache-test");

    local_tcp_session_init();
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_queue_telemetry(&snapshot));
    TEST_CHECK(APP_STATUS_UNAVAILABLE == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_open_link(4U));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(4U, hello, hello_bytes));

    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(outbound.control_response);
    TEST_CHECK(RA8P_MESSAGE_HELLO_ACK ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(payload_contains(&outbound, "\"build_version\":\"offline-build\""));
    TEST_CHECK(payload_contains(&outbound, "\"session_id\":\"boot-cache-test\""));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    local_tcp_session_consume_outbound(true, true);

    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(!outbound.control_response);
    TEST_CHECK(RA8P_MESSAGE_STATE_SNAPSHOT ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(300U == read_u32_be(&outbound.p_data[RA8P_HEADER_SEQUENCE_OFFSET]));
    TEST_CHECK(payload_contains(&outbound, "\"environment_valid\":true"));
    TEST_CHECK(payload_contains(&outbound, "\"light_valid\":true"));
    TEST_CHECK(payload_contains(&outbound, "\"time_valid\":true"));
    TEST_CHECK(payload_contains(&outbound, "\"wall_time_ms\":1786900000123"));
    TEST_CHECK(payload_contains(&outbound, "\"target_position_mm\":1200"));
    TEST_CHECK(payload_contains(&outbound, "\"actual_position_mm\":-25"));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(false));
    local_tcp_session_consume_outbound(false, true);

    snapshot.sequence = 301U;
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_queue_telemetry(&snapshot));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(RA8P_MESSAGE_TELEMETRY ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(false));
    local_tcp_session_consume_outbound(false, true);

    local_tcp_session_close_link(4U);
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_open_link(0U));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(0U, hello, hello_bytes));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    local_tcp_session_consume_outbound(true, true);
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(RA8P_MESSAGE_STATE_SNAPSHOT ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(301U == read_u32_be(&outbound.p_data[RA8P_HEADER_SEQUENCE_OFFSET]));

    local_tcp_session_get_diagnostics(&diagnostics);
    TEST_CHECK(2U == diagnostics.links_accepted);
    TEST_CHECK(2U == diagnostics.snapshots_queued);
    TEST_CHECK(1U == diagnostics.snapshots_sent);
    TEST_CHECK(diagnostics.snapshot_pending);
    TEST_CHECK(2U == diagnostics.telemetry_cached);
    return 0;
}

static int test_realistic_embedded_snapshot_values (void)
{
    uint8_t hello[128];
    uint32_t hello_bytes = 0U;
    app_telemetry_snapshot_t snapshot = {0};
    local_tcp_outbound_t outbound;
    local_tcp_session_diagnostics_t diagnostics;

    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_HELLO, 50U, RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{}", 2U, hello, sizeof(hello), &hello_bytes));

    snapshot.schema_version = APP_TELEMETRY_SCHEMA_VERSION;
    snapshot.sequence = UINT32_MAX;
    snapshot.monotonic_us = UINT64_MAX;
    snapshot.wall_time_ms = UINT64_MAX;
    snapshot.wall_time_valid = true;
    snapshot.capabilities = UINT32_MAX;
    snapshot.temperature_milli_c = INT32_MIN;
    snapshot.humidity_milli_percent = UINT32_MAX;
    snapshot.environment_age_ms = UINT32_MAX;
    snapshot.light_raw = UINT16_MAX;
    snapshot.light_average = UINT16_MAX;
    snapshot.light_tenths_percent = UINT16_MAX;
    snapshot.wheat_count = INT32_MAX;
    snapshot.inference_us = UINT32_MAX;
    snapshot.target_position_mm = UINT32_MAX;
    snapshot.actual_position_mm = INT32_MIN;
    snapshot.fault_flags = UINT32_MAX;
    (void) strcpy(snapshot.device_id, "ra8p1-cpkhmi-v1");
    (void) strcpy(snapshot.session_id, "boot-ffffffff-ffffffff");
    (void) strcpy(snapshot.build_version, "release-fsp-6.5.0");
    (void) strcpy(snapshot.model_version, "yolox-tiny-int8-tile224");

    local_tcp_session_init();
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_queue_telemetry(&snapshot));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_open_link(1U));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(1U, hello, hello_bytes));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    local_tcp_session_consume_outbound(true, true);

    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(!outbound.control_response);
    TEST_CHECK(RA8P_MESSAGE_STATE_SNAPSHOT ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(payload_contains(&outbound,
                                "\"monotonic_us\":18446744073709551615"));
    TEST_CHECK(payload_contains(&outbound,
                                "\"wall_time_ms\":18446744073709551615"));
    TEST_CHECK(payload_contains(&outbound,
                                "\"temperature_c\":-2147483.648"));
    TEST_CHECK(payload_contains(&outbound,
                                "\"inference_ms\":4294967.295"));

    local_tcp_session_get_diagnostics(&diagnostics);
    TEST_CHECK(0U == diagnostics.snapshot_format_errors);
    TEST_CHECK(0U == diagnostics.snapshot_encode_errors);
    TEST_CHECK(APP_STATUS_OK == diagnostics.last_snapshot_status);
    TEST_CHECK(0U == diagnostics.protocol_errors);
    return 0;
}

static int test_hello_ack_survives_snapshot_format_error (void)
{
    uint8_t hello[128];
    uint32_t hello_bytes = 0U;
    app_telemetry_snapshot_t snapshot = {0};
    local_tcp_outbound_t outbound;
    local_tcp_session_diagnostics_t diagnostics;

    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_HELLO, 60U, RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{}", 2U, hello, sizeof(hello), &hello_bytes));

    snapshot.schema_version = APP_TELEMETRY_SCHEMA_VERSION;
    snapshot.sequence = 600U;
    (void) strcpy(snapshot.device_id, "ra8p1-format-test");
    (void) strcpy(snapshot.session_id, "boot-format-test");
    (void) strcpy(snapshot.build_version, "test-build");
    (void) memset(snapshot.model_version, 'X', sizeof(snapshot.model_version));

    local_tcp_session_init();
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_queue_telemetry(&snapshot));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_open_link(2U));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_feed(2U, hello, hello_bytes));

    local_tcp_session_get_diagnostics(&diagnostics);
    TEST_CHECK(diagnostics.negotiated);
    TEST_CHECK(diagnostics.snapshot_pending);
    TEST_CHECK(1U == diagnostics.responses_queued);
    TEST_CHECK(0U == diagnostics.snapshots_queued);
    TEST_CHECK(1U == diagnostics.snapshot_format_errors);
    TEST_CHECK(0U == diagnostics.snapshot_encode_errors);
    TEST_CHECK(APP_STATUS_DATA == diagnostics.last_snapshot_status);
    TEST_CHECK(0U == diagnostics.protocol_errors);

    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(outbound.control_response);
    TEST_CHECK(RA8P_MESSAGE_HELLO_ACK ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_mark_outbound_in_flight(true));
    local_tcp_session_consume_outbound(true, true);
    TEST_CHECK(APP_STATUS_UNAVAILABLE == local_tcp_session_peek_outbound(&outbound));

    (void) strcpy(snapshot.model_version, "recovered-model");
    snapshot.sequence++;
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_queue_telemetry(&snapshot));
    TEST_CHECK(APP_STATUS_OK == local_tcp_session_peek_outbound(&outbound));
    TEST_CHECK(!outbound.control_response);
    TEST_CHECK(RA8P_MESSAGE_STATE_SNAPSHOT ==
               read_u16_be(&outbound.p_data[RA8P_HEADER_TYPE_OFFSET]));
    TEST_CHECK(601U == read_u32_be(&outbound.p_data[RA8P_HEADER_SEQUENCE_OFFSET]));
    TEST_CHECK(payload_contains(&outbound,
                                "\"model_version\":\"recovered-model\""));

    local_tcp_session_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.snapshot_format_errors);
    TEST_CHECK(1U == diagnostics.snapshots_queued);
    TEST_CHECK(APP_STATUS_OK == diagnostics.last_snapshot_status);
    TEST_CHECK(0U == diagnostics.protocol_errors);
    return 0;
}

int main (void)
{
    TEST_CHECK(0 == test_handshake_ping_and_fragmentation());
    TEST_CHECK(0 == test_single_link_and_queue_priorities());
    TEST_CHECK(0 == test_in_flight_buffer_ownership());
    TEST_CHECK(0 == test_offline_cache_and_reconnect_snapshot());
    TEST_CHECK(0 == test_realistic_embedded_snapshot_values());
    TEST_CHECK(0 == test_hello_ack_survives_snapshot_format_error());
    (void) printf("local-tcp-session status=ok split=ok sticky=ok single-link=ok "
                  "snapshot-resync=ok latest-telemetry=ok ownership=locked "
                  "embedded-values=ok snapshot-failure-isolated=ok\n");
    return 0;
}

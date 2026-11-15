#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lan_session_router.h"
#include "web_ui_asset.h"
#include "wire_protocol.h"

#define TEST_CHECK(condition)                                                        \
    do                                                                               \
    {                                                                                \
        if (!(condition))                                                            \
        {                                                                            \
            (void) fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                \
        }                                                                            \
    } while (0)

static uint32_t make_masked_text(char const * p_text, uint8_t * p_output);
static void make_snapshot(app_telemetry_snapshot_t * p_snapshot, uint32_t sequence);
static int send_next(uint8_t expected_link, bool expected_control);
static int websocket_connect(uint8_t link_id);
static int test_http_and_websocket_sessions(void);
static int test_legacy_same_port(void);

static uint32_t make_masked_text (char const * p_text, uint8_t * p_output)
{
    static uint8_t const mask[4] = {1U, 2U, 3U, 4U};
    uint32_t const text_bytes = (uint32_t) strlen(p_text);
    uint32_t index;

    p_output[0] = 0x81U;
    p_output[1] = (uint8_t) (0x80U | text_bytes);
    (void) memcpy(&p_output[2], mask, sizeof(mask));
    for (index = 0U; index < text_bytes; index++)
    {
        p_output[6U + index] = (uint8_t) p_text[index] ^ mask[index & 3U];
    }
    return text_bytes + 6U;
}

static void make_snapshot (app_telemetry_snapshot_t * p_snapshot, uint32_t sequence)
{
    app_telemetry_snapshot_t snapshot = {0};

    snapshot.schema_version = APP_TELEMETRY_SCHEMA_VERSION;
    snapshot.sequence = sequence;
    snapshot.temperature_milli_c = 23500;
    snapshot.humidity_milli_percent = 61250U;
    snapshot.light_tenths_percent = 585U;
    snapshot.wheat_count = 42;
    snapshot.inference_us = 185000U;
    snapshot.environment_valid = true;
    snapshot.light_valid = true;
    (void) strcpy(snapshot.device_id, "ra8p1-router-test");
    (void) strcpy(snapshot.session_id, "boot-router");
    (void) strcpy(snapshot.build_version, "test");
    (void) strcpy(snapshot.model_version, "yolox-test");
    *p_snapshot = snapshot;
}

static int send_next (uint8_t expected_link, bool expected_control)
{
    lan_session_outbound_t outbound;

    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(expected_link == outbound.link_id);
    TEST_CHECK(expected_control == outbound.control_response);
    TEST_CHECK(APP_STATUS_OK == lan_session_router_mark_outbound_in_flight(
        outbound.link_id, outbound.control_response));
    lan_session_router_consume_outbound(outbound.link_id, outbound.control_response, true);
    return 0;
}

static int websocket_connect (uint8_t link_id)
{
    static char const request[] =
        "GET /ws HTTP/1.1\r\nHost: device\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    lan_session_outbound_t outbound;

    TEST_CHECK(APP_STATUS_OK == lan_session_router_open_link(link_id));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_feed(
        link_id, (uint8_t const *) request, (uint32_t) strlen(request)));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(link_id == outbound.link_id);
    TEST_CHECK(outbound.control_response);
    TEST_CHECK(NULL != strstr((char const *) outbound.p_data,
                              "101 Switching Protocols"));
    TEST_CHECK(NULL != strstr((char const *) outbound.p_data,
                              "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_mark_outbound_in_flight(
        link_id, true));
    lan_session_router_consume_outbound(link_id, true, true);
    return 0;
}

static int test_http_and_websocket_sessions (void)
{
    static char const root[] = "GET / HTTP/1.1\r\nHost: device\r\n\r\n";
    static char const health[] = "GET /health HTTP/1.1\r\nHost: device\r\n\r\n";
    app_telemetry_snapshot_t snapshot;
    lan_session_router_diagnostics_t diagnostics;
    web_ui_asset_info_t asset_info;
    lan_session_outbound_t outbound;
    uint8_t command[128];
    uint8_t sticky_commands[256];
    uint32_t command_bytes;
    uint32_t root_sends = 0U;

    make_snapshot(&snapshot, 10U);
    lan_session_router_init();
    lan_session_router_get_diagnostics(&diagnostics);
    if (!diagnostics.web_asset_valid)
    {
        web_ui_asset_get_info(&asset_info);
        (void) printf("web-asset actual_crc=0x%08lx status=%u\n",
                      (unsigned long) asset_info.actual_crc32,
                      (unsigned int) diagnostics.web_asset_status);
    }
    web_ui_asset_get_info(&asset_info);
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "SD 卡"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "终端已连接"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "DHT11：约 1 秒"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "光敏：约 100 ms"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "环境检测"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "小麦识别与整田计数"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "试验田动态拍摄分布地图"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "35 cm 小车过道"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "底盘运动与田间参数"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "右侧滑轨与拍摄设置"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "MissionPlanV1"));
    TEST_CHECK(NULL != strstr((char const *) asset_info.p_page, "id='captureinterval'"));
    TEST_CHECK(asset_info.page_bytes <= (40U * 1024U));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_queue_telemetry(&snapshot));

    TEST_CHECK(APP_STATUS_OK == lan_session_router_open_link(0U));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_feed(
        0U, (uint8_t const *) root, (uint32_t) strlen(root)));
    do
    {
        TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
        TEST_CHECK(0U == outbound.link_id);
        TEST_CHECK(outbound.control_response);
        TEST_CHECK(APP_STATUS_OK == lan_session_router_mark_outbound_in_flight(0U, true));
        lan_session_router_consume_outbound(0U, true, true);
        root_sends++;
        TEST_CHECK(root_sends < ((asset_info.page_bytes / 1024U) + 4U));
    } while (!lan_session_router_should_close(0U));
    TEST_CHECK(root_sends > 2U);
    lan_session_router_close_link(0U);

    TEST_CHECK(APP_STATUS_OK == lan_session_router_open_link(1U));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_feed(
        1U, (uint8_t const *) health, (uint32_t) strlen(health)));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(NULL != strstr((char const *) outbound.p_data, "application/json"));
    TEST_CHECK(NULL != strstr((char const *) outbound.p_data, "\"sequence\":10"));
    TEST_CHECK(0 == send_next(1U, true));
    TEST_CHECK(lan_session_router_should_close(1U));
    lan_session_router_close_link(1U);

    TEST_CHECK(0 == websocket_connect(2U));
    snapshot.sequence = 11U;
    TEST_CHECK(APP_STATUS_OK == lan_session_router_queue_telemetry(&snapshot));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(2U == outbound.link_id);
    TEST_CHECK(!outbound.control_response);
    TEST_CHECK(NULL != strstr((char const *) &outbound.p_data[2], "\"control\":true"));
    TEST_CHECK(NULL != strstr((char const *) &outbound.p_data[4], "\"sequence\":11"));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_mark_outbound_in_flight(2U, false));
    snapshot.sequence = 12U;
    TEST_CHECK(APP_STATUS_OK == lan_session_router_queue_telemetry(&snapshot));
    snapshot.sequence = 13U;
    TEST_CHECK(APP_STATUS_OK == lan_session_router_queue_telemetry(&snapshot));
    lan_session_router_consume_outbound(2U, false, true);
    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(NULL != strstr((char const *) &outbound.p_data[4], "\"type\":\"telemetry\""));
    TEST_CHECK(NULL != strstr((char const *) &outbound.p_data[4], "\"sequence\":13"));
    TEST_CHECK(0 == send_next(2U, false));

    TEST_CHECK(0 == websocket_connect(3U));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(3U == outbound.link_id);
    TEST_CHECK(!outbound.control_response);
    TEST_CHECK(NULL != strstr((char const *) &outbound.p_data[2], "\"control\":false"));
    TEST_CHECK(0 == send_next(3U, false));

    command_bytes = make_masked_text("{\"type\":\"mission_request\"}", command);
    TEST_CHECK(APP_STATUS_OK == lan_session_router_feed(3U, command, command_bytes));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(3U == outbound.link_id);
    TEST_CHECK(NULL != strstr((char const *) &outbound.p_data[2], "read_only"));
    TEST_CHECK(0 == send_next(3U, true));

    (void) memcpy(sticky_commands, command, command_bytes);
    (void) memcpy(&sticky_commands[command_bytes], command, command_bytes);
    TEST_CHECK(APP_STATUS_OK == lan_session_router_feed(
        3U, sticky_commands, command_bytes * 2U));
    TEST_CHECK(0 == send_next(3U, true));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(3U == outbound.link_id);
    TEST_CHECK(outbound.control_response);
    TEST_CHECK(0 == send_next(3U, true));

    TEST_CHECK(APP_STATUS_OK == lan_session_router_feed(2U, command, command_bytes));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(2U == outbound.link_id);
    TEST_CHECK(NULL != strstr((char const *) &outbound.p_data[2], "unsupported"));
    TEST_CHECK(0 == send_next(2U, true));

    lan_session_router_close_link(2U);
    lan_session_router_get_diagnostics(&diagnostics);
    TEST_CHECK(LAN_SESSION_ROUTER_NO_LINK == diagnostics.controller_link_id);
    TEST_CHECK(1U == diagnostics.lease_releases);
    TEST_CHECK(3U == diagnostics.observer_commands_rejected);
    TEST_CHECK(1U == diagnostics.controller_commands_rejected);
    lan_session_router_close_link(3U);
    return 0;
}

static int test_legacy_same_port (void)
{
    uint8_t hello[128];
    uint32_t hello_bytes = 0U;
    lan_session_outbound_t outbound;

    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_HELLO, 7U, RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{}", 2U, hello, sizeof(hello), &hello_bytes));
    lan_session_router_init();
    TEST_CHECK(APP_STATUS_OK == lan_session_router_open_link(4U));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_feed(4U, hello, hello_bytes));
    TEST_CHECK(APP_STATUS_OK == lan_session_router_peek_outbound(&outbound));
    TEST_CHECK(4U == outbound.link_id);
    TEST_CHECK(outbound.control_response);
    TEST_CHECK(0 == memcmp(outbound.p_data, "RA8P", 4U));
    TEST_CHECK(0 == send_next(4U, true));
    lan_session_router_close_link(4U);
    return 0;
}

int main (void)
{
    TEST_CHECK(0 == test_http_and_websocket_sessions());
    TEST_CHECK(0 == test_legacy_same_port());
    (void) printf("lan-router status=ok links=5 http=chunked websocket=controller-lease "
                  "observer=read-only telemetry=latest legacy=same-port\n");
    return 0;
}

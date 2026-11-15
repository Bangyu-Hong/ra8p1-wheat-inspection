#include "esp_at_service.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_at_reset.h"

#if defined(ESP_AT_CREDENTIALS_HOST_TEST)
 #define ESP_AT_WIFI_CREDENTIALS_CONFIGURED  (1)
 #define ESP_AT_WIFI_SSID                    "TestNet"
 #define ESP_AT_WIFI_PASSWORD                "secret"
#elif defined(ESP_AT_NO_CREDENTIALS_HOST_TEST)
 #define ESP_AT_WIFI_CREDENTIALS_CONFIGURED  (0)
 #define ESP_AT_WIFI_SSID                    ""
 #define ESP_AT_WIFI_PASSWORD                ""
#elif defined(__has_include)
 #if __has_include("esp_at_credentials.h")
  #include "esp_at_credentials.h"
 #endif
#endif

#ifndef ESP_AT_WIFI_CREDENTIALS_CONFIGURED
 #define ESP_AT_WIFI_CREDENTIALS_CONFIGURED  (0)
#endif
#ifndef ESP_AT_WIFI_SSID
 #define ESP_AT_WIFI_SSID                    ""
#endif
#ifndef ESP_AT_WIFI_PASSWORD
 #define ESP_AT_WIFI_PASSWORD                ""
#endif

#define ESP_AT_SERVICE_POWER_WAIT_US       (500000ULL)
#define ESP_AT_SERVICE_RESET_HOLD_US       (200000ULL)
#define ESP_AT_SERVICE_RESET_BOOT_WAIT_US  (1800000ULL)
#define ESP_AT_SERVICE_RESET_THRESHOLD     (3U)
#define ESP_AT_SERVICE_COMMAND_TIMEOUT_US  (2000000ULL)
#define ESP_AT_SERVICE_JOIN_TIMEOUT_US     (20000000ULL)
#define ESP_AT_SERVICE_IP_RETRY_WAIT_US    (500000ULL)
#define ESP_AT_SERVICE_IP_QUERY_ATTEMPTS   (6U)
#define ESP_AT_SERVICE_SEND_TIMEOUT_US     (5000000ULL)
#define ESP_AT_SERVICE_BACKOFF_MIN_US      (1000000ULL)
#define ESP_AT_SERVICE_BACKOFF_MAX_US      (30000000ULL)
#define ESP_AT_SERVICE_RX_CHUNK_BYTES      (512U)
#define ESP_AT_SERVICE_COMMAND_BYTES       (256U)
#define ESP_AT_SERVICE_MAX_LINK_ID         (4U)
#define ESP_AT_SERVICE_NO_LINK             (0xFFU)

typedef enum e_esp_at_io_state
{
    ESP_AT_IO_IDLE = 0,
    ESP_AT_IO_SEND_WAIT_PROMPT,
    ESP_AT_IO_SEND_WAIT_RESULT,
    ESP_AT_IO_CLOSE_WAIT
} esp_at_io_state_t;

static esp_at_parser_t g_parser;
static esp_at_service_diagnostics_t g_diagnostics;
static uint64_t g_deadline_us;
static uint64_t g_backoff_us;
static uint64_t g_now_us;
static esp_at_io_state_t g_io_state;
static uint8_t g_pending_close_mask;
static uint8_t g_close_link;
static uint8_t g_send_link;
static bool g_send_control_response;
static bool g_send_buffer_locked;
static bool g_reset_after_backoff;
static uint32_t g_recoveries_since_reset;
static uint32_t g_seen_rx_overflows;
static uint32_t g_ip_query_attempts;
static uint32_t g_command_tx_completion_target;
static bool g_command_response_armed;
static uint8_t const * g_send_data;
static uint32_t g_send_bytes;

static app_status_t esp_at_service_line_handler(char const * p_line,
                                                uint32_t line_bytes,
                                                void * p_context);
static app_status_t esp_at_service_ipd_handler(uint8_t link_id,
                                               uint8_t const * p_data,
                                               uint32_t data_bytes,
                                               void * p_context);
static app_status_t esp_at_service_send_text(char const * p_text);
static app_status_t esp_at_service_send_join(void);
static app_status_t esp_at_service_start_outbound(void);
static app_status_t esp_at_service_send_outbound_payload(void);
static app_status_t esp_at_service_send_pending_close(void);
static void esp_at_service_process_listening_io(void);
static void esp_at_service_check_timeouts(uint64_t now_us);
static uint8_t esp_at_service_next_pending_close(void);
static void esp_at_service_transition(esp_at_service_state_t state,
                                      uint64_t timeout_us);
static void esp_at_service_enter_recovery(app_status_t status);
static app_status_t esp_at_service_set_reset(bool asserted);
static void esp_at_service_finish_outbound(bool sent);
static void esp_at_service_close_all_links(void);
static void esp_at_service_request_link_close(uint8_t link_id);
static bool esp_at_service_line_equals(char const * p_line,
                                       uint32_t line_bytes,
                                       char const * p_expected);
static bool esp_at_service_line_ends_with(char const * p_line,
                                          uint32_t line_bytes,
                                          char const * p_suffix);
static bool esp_at_service_parse_link_event(char const * p_line,
                                            uint32_t line_bytes,
                                            char const * p_suffix,
                                            uint8_t * p_link_id);
static bool esp_at_service_append_escaped(char * p_output,
                                          size_t output_bytes,
                                          size_t * p_used,
                                          char const * p_text);
static void esp_at_service_capture_station_ip(char const * p_line,
                                              uint32_t line_bytes);
static bool esp_at_service_parse_ipv4(char const * p_text,
                                      uint32_t text_bytes);
static void esp_at_service_capture_version(char const * p_line,
                                           uint32_t line_bytes);
static bool esp_at_service_waiting_for_response(esp_at_service_state_t state);
static esp_at_network_state_t esp_at_service_network_state(void);
static app_status_t esp_at_service_record(app_status_t status);

app_status_t esp_at_service_init (uint64_t now_us)
{
    app_status_t status;

    if (g_diagnostics.initialized)
    {
        return esp_at_service_record(APP_STATUS_OK);
    }

    esp_at_parser_init(&g_parser);
    lan_session_router_init();
    g_diagnostics = (esp_at_service_diagnostics_t) {0};
    g_diagnostics.state = ESP_AT_SERVICE_STATE_CLOSED;
    g_diagnostics.network_state = ESP_AT_NETWORK_OFFLINE;
    g_diagnostics.active_link_id = ESP_AT_SERVICE_NO_LINK;
    g_diagnostics.credentials_configured =
        (0 != ESP_AT_WIFI_CREDENTIALS_CONFIGURED) &&
        ('\0' != ESP_AT_WIFI_SSID[0]);
    g_deadline_us = 0U;
    g_backoff_us = ESP_AT_SERVICE_BACKOFF_MIN_US;
    g_now_us = now_us;
    g_io_state = ESP_AT_IO_IDLE;
    g_pending_close_mask = 0U;
    g_close_link = ESP_AT_SERVICE_NO_LINK;
    g_send_link = ESP_AT_SERVICE_NO_LINK;
    g_send_control_response = false;
    g_send_buffer_locked = false;
    g_reset_after_backoff = false;
    g_recoveries_since_reset = 0U;
    g_seen_rx_overflows = 0U;
    g_ip_query_attempts = 0U;
    g_command_tx_completion_target = 0U;
    g_command_response_armed = false;
    g_send_data = NULL;
    g_send_bytes = 0U;

    status = esp_at_transport_init();
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.command_errors++;
        return esp_at_service_record(status);
    }
    g_diagnostics.initialized = true;
    esp_at_service_transition(ESP_AT_SERVICE_STATE_RESET_ASSERT, 0U);
    return esp_at_service_record(APP_STATUS_OK);
}

app_status_t esp_at_service_process (uint64_t now_us)
{
    uint8_t rx_data[ESP_AT_SERVICE_RX_CHUNK_BYTES];
    uint32_t rx_bytes = 0U;
    app_status_t status;

    if (!g_diagnostics.initialized)
    {
        return esp_at_service_record(APP_STATUS_NOT_INITIALIZED);
    }
    if (ESP_AT_SERVICE_STATE_RESET_FAULT == g_diagnostics.state)
    {
        return g_diagnostics.last_status;
    }
    g_now_us = now_us;

    status = esp_at_transport_process(now_us);
    if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
    {
        esp_at_service_enter_recovery(status);
    }
    esp_at_transport_get_diagnostics(&g_diagnostics.transport);
    if (esp_at_service_waiting_for_response(g_diagnostics.state) &&
        !g_command_response_armed &&
        (g_diagnostics.transport.tx_completed_requests >=
         g_command_tx_completion_target))
    {
        g_command_response_armed = true;
    }
    if (g_seen_rx_overflows != g_diagnostics.transport.rx_overflows)
    {
        g_seen_rx_overflows = g_diagnostics.transport.rx_overflows;
        g_diagnostics.parser_errors++;
        esp_at_transport_discard_rx();
        esp_at_parser_init(&g_parser);
        esp_at_service_enter_recovery(APP_STATUS_UNAVAILABLE);
        return esp_at_service_record(APP_STATUS_OK);
    }

    status = esp_at_transport_read(rx_data, sizeof(rx_data), &rx_bytes);
    if (APP_STATUS_OK != status)
    {
        esp_at_service_enter_recovery(status);
        return esp_at_service_record(status);
    }
    if (0U < rx_bytes)
    {
        status = esp_at_parser_feed(&g_parser,
                                    rx_data,
                                    rx_bytes,
                                    esp_at_service_line_handler,
                                    esp_at_service_ipd_handler,
                                    NULL);
        if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
        {
            g_diagnostics.parser_errors++;
        }
        g_diagnostics.network_state = esp_at_service_network_state();
        lan_session_router_get_diagnostics(&g_diagnostics.session);
        g_diagnostics.active_link_id = g_diagnostics.session.controller_link_id;
        esp_at_service_check_timeouts(now_us);
        if (ESP_AT_SERVICE_STATE_LISTENING == g_diagnostics.state)
        {
            esp_at_service_process_listening_io();
        }
        return esp_at_service_record(APP_STATUS_OK);
    }

    esp_at_service_check_timeouts(now_us);

    switch (g_diagnostics.state)
    {
        case ESP_AT_SERVICE_STATE_RESET_ASSERT:
            status = esp_at_service_set_reset(true);
            if (APP_STATUS_OK != status)
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_RESET_FAULT, 0U);
                return esp_at_service_record(status);
            }
            g_diagnostics.reset_count++;
            g_recoveries_since_reset = 0U;
            esp_at_service_transition(ESP_AT_SERVICE_STATE_RESET_HOLD,
                                      ESP_AT_SERVICE_RESET_HOLD_US);
            break;

        case ESP_AT_SERVICE_STATE_RESET_HOLD:
            if (now_us >= g_deadline_us)
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_RESET_RELEASE, 0U);
            }
            break;

        case ESP_AT_SERVICE_STATE_RESET_RELEASE:
            status = esp_at_service_set_reset(false);
            if (APP_STATUS_OK != status)
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_RESET_FAULT, 0U);
                return esp_at_service_record(status);
            }
            esp_at_parser_init(&g_parser);
            esp_at_service_transition(ESP_AT_SERVICE_STATE_RESET_BOOT_WAIT,
                                      ESP_AT_SERVICE_RESET_BOOT_WAIT_US);
            break;

        case ESP_AT_SERVICE_STATE_RESET_BOOT_WAIT:
            if (now_us >= g_deadline_us)
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_PROBE_SEND, 0U);
            }
            break;

        case ESP_AT_SERVICE_STATE_POWER_WAIT:
            if (now_us >= g_deadline_us)
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_PROBE_SEND, 0U);
            }
            break;

        case ESP_AT_SERVICE_STATE_PROBE_SEND:
            if (APP_STATUS_OK == esp_at_service_send_text("AT\r\n"))
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_PROBE_WAIT,
                                          ESP_AT_SERVICE_COMMAND_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_ECHO_SEND:
            if (APP_STATUS_OK == esp_at_service_send_text("ATE0\r\n"))
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_ECHO_WAIT,
                                          ESP_AT_SERVICE_COMMAND_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_VERSION_SEND:
            if (APP_STATUS_OK == esp_at_service_send_text("AT+GMR\r\n"))
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_VERSION_WAIT,
                                          ESP_AT_SERVICE_COMMAND_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_WIFI_MODE_SEND:
            if (APP_STATUS_OK == esp_at_service_send_text("AT+CWMODE=1\r\n"))
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_WIFI_MODE_WAIT,
                                          ESP_AT_SERVICE_COMMAND_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_JOIN_SEND:
            if (APP_STATUS_OK == esp_at_service_send_join())
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_JOIN_WAIT,
                                          ESP_AT_SERVICE_JOIN_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_IP_QUERY_SEND:
            g_diagnostics.station_ip[0] = '\0';
            if (APP_STATUS_OK == esp_at_service_send_text("AT+CIFSR\r\n"))
            {
                g_ip_query_attempts++;
                esp_at_service_transition(ESP_AT_SERVICE_STATE_IP_QUERY_WAIT,
                                          ESP_AT_SERVICE_COMMAND_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_IP_QUERY_RETRY_WAIT:
            if (now_us >= g_deadline_us)
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_IP_QUERY_SEND, 0U);
            }
            break;

        case ESP_AT_SERVICE_STATE_MUX_SEND:
            if (APP_STATUS_OK == esp_at_service_send_text("AT+CIPMUX=1\r\n"))
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_MUX_WAIT,
                                          ESP_AT_SERVICE_COMMAND_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_SEND:
            if (APP_STATUS_OK == esp_at_service_send_text("AT+CIPSERVERMAXCONN=5\r\n"))
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_WAIT,
                                          ESP_AT_SERVICE_COMMAND_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_SERVER_STOP_SEND:
            if (APP_STATUS_OK == esp_at_service_send_text("AT+CIPSERVER=0\r\n"))
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_SERVER_STOP_WAIT,
                                          ESP_AT_SERVICE_COMMAND_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_SERVER_START_SEND:
            if (APP_STATUS_OK == esp_at_service_send_text("AT+CIPSERVER=1,80\r\n"))
            {
                esp_at_service_transition(ESP_AT_SERVICE_STATE_SERVER_START_WAIT,
                                          ESP_AT_SERVICE_COMMAND_TIMEOUT_US);
            }
            break;

        case ESP_AT_SERVICE_STATE_LISTENING:
            esp_at_service_process_listening_io();
            break;

        case ESP_AT_SERVICE_STATE_BACKOFF:
            if (now_us >= g_deadline_us)
            {
                esp_at_parser_init(&g_parser);
                if (g_reset_after_backoff)
                {
                    g_reset_after_backoff = false;
                    esp_at_service_transition(ESP_AT_SERVICE_STATE_RESET_ASSERT, 0U);
                }
                else
                {
                    esp_at_service_transition(ESP_AT_SERVICE_STATE_PROBE_SEND, 0U);
                }
            }
            break;

        case ESP_AT_SERVICE_STATE_CLOSED:
        case ESP_AT_SERVICE_STATE_RESET_FAULT:
        case ESP_AT_SERVICE_STATE_PROBE_WAIT:
        case ESP_AT_SERVICE_STATE_ECHO_WAIT:
        case ESP_AT_SERVICE_STATE_VERSION_WAIT:
        case ESP_AT_SERVICE_STATE_CREDENTIALS_WAIT:
        case ESP_AT_SERVICE_STATE_WIFI_MODE_WAIT:
        case ESP_AT_SERVICE_STATE_JOIN_WAIT:
        case ESP_AT_SERVICE_STATE_IP_QUERY_WAIT:
        case ESP_AT_SERVICE_STATE_MUX_WAIT:
        case ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_WAIT:
        case ESP_AT_SERVICE_STATE_SERVER_STOP_WAIT:
        case ESP_AT_SERVICE_STATE_SERVER_START_WAIT:
        default:
            break;
    }

    g_diagnostics.network_state = esp_at_service_network_state();
    lan_session_router_get_diagnostics(&g_diagnostics.session);
    g_diagnostics.active_link_id = g_diagnostics.session.controller_link_id;
    return esp_at_service_record(APP_STATUS_OK);
}

app_status_t esp_at_service_queue_telemetry (app_telemetry_snapshot_t const * p_snapshot)
{
    app_status_t status;

    if (!g_diagnostics.initialized)
    {
        return esp_at_service_record(APP_STATUS_NOT_INITIALIZED);
    }
    status = lan_session_router_queue_telemetry(p_snapshot);
    if (APP_STATUS_BUSY == status)
    {
        g_diagnostics.telemetry_busy++;
    }
    return esp_at_service_record(status);
}

app_status_t esp_at_service_close (void)
{
    app_status_t reset_status;
    app_status_t transport_status = APP_STATUS_OK;

    reset_status = esp_at_service_set_reset(false);
    if (g_diagnostics.initialized)
    {
        esp_at_service_finish_outbound(false);
        esp_at_service_close_all_links();
        transport_status = esp_at_transport_close();
    }
    if (APP_STATUS_OK == transport_status)
    {
        g_diagnostics.initialized = false;
        g_diagnostics.server_listening = false;
        g_diagnostics.wifi_connected = false;
        g_diagnostics.got_ip = false;
        g_diagnostics.state = (APP_STATUS_OK == reset_status) ?
                              ESP_AT_SERVICE_STATE_CLOSED :
                              ESP_AT_SERVICE_STATE_RESET_FAULT;
        g_diagnostics.network_state = ESP_AT_NETWORK_OFFLINE;
    }
    if (APP_STATUS_OK != reset_status)
    {
        return esp_at_service_record(reset_status);
    }
    return esp_at_service_record(transport_status);
}

void esp_at_service_get_diagnostics (esp_at_service_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        esp_at_parser_get_diagnostics(&g_parser, &g_diagnostics.parser);
        esp_at_transport_get_diagnostics(&g_diagnostics.transport);
        lan_session_router_get_diagnostics(&g_diagnostics.session);
        g_diagnostics.active_link_id = g_diagnostics.session.controller_link_id;
        g_diagnostics.network_state = esp_at_service_network_state();
        g_diagnostics.send_in_progress =
            (ESP_AT_IO_SEND_WAIT_PROMPT == g_io_state) ||
            (ESP_AT_IO_SEND_WAIT_RESULT == g_io_state);
        *p_diagnostics = g_diagnostics;
    }
}

char const * esp_at_service_state_name (esp_at_service_state_t state)
{
    switch (state)
    {
        case ESP_AT_SERVICE_STATE_CLOSED: return "closed";
        case ESP_AT_SERVICE_STATE_RESET_ASSERT: return "reset-assert";
        case ESP_AT_SERVICE_STATE_RESET_HOLD: return "reset-hold";
        case ESP_AT_SERVICE_STATE_RESET_RELEASE: return "reset-release";
        case ESP_AT_SERVICE_STATE_RESET_BOOT_WAIT: return "reset-boot-wait";
        case ESP_AT_SERVICE_STATE_RESET_FAULT: return "reset-fault";
        case ESP_AT_SERVICE_STATE_POWER_WAIT: return "power-wait";
        case ESP_AT_SERVICE_STATE_PROBE_SEND: return "probe-send";
        case ESP_AT_SERVICE_STATE_PROBE_WAIT: return "probe-wait";
        case ESP_AT_SERVICE_STATE_ECHO_SEND: return "echo-send";
        case ESP_AT_SERVICE_STATE_ECHO_WAIT: return "echo-wait";
        case ESP_AT_SERVICE_STATE_VERSION_SEND: return "version-send";
        case ESP_AT_SERVICE_STATE_VERSION_WAIT: return "version-wait";
        case ESP_AT_SERVICE_STATE_CREDENTIALS_WAIT: return "credentials-wait";
        case ESP_AT_SERVICE_STATE_WIFI_MODE_SEND: return "wifi-mode-send";
        case ESP_AT_SERVICE_STATE_WIFI_MODE_WAIT: return "wifi-mode-wait";
        case ESP_AT_SERVICE_STATE_JOIN_SEND: return "join-send";
        case ESP_AT_SERVICE_STATE_JOIN_WAIT: return "join-wait";
        case ESP_AT_SERVICE_STATE_IP_QUERY_SEND: return "ip-query-send";
        case ESP_AT_SERVICE_STATE_IP_QUERY_WAIT: return "ip-query-wait";
        case ESP_AT_SERVICE_STATE_IP_QUERY_RETRY_WAIT: return "ip-query-retry-wait";
        case ESP_AT_SERVICE_STATE_MUX_SEND: return "mux-send";
        case ESP_AT_SERVICE_STATE_MUX_WAIT: return "mux-wait";
        case ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_SEND: return "max-connections-send";
        case ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_WAIT: return "max-connections-wait";
        case ESP_AT_SERVICE_STATE_SERVER_STOP_SEND: return "server-stop-send";
        case ESP_AT_SERVICE_STATE_SERVER_STOP_WAIT: return "server-stop-wait";
        case ESP_AT_SERVICE_STATE_SERVER_START_SEND: return "server-start-send";
        case ESP_AT_SERVICE_STATE_SERVER_START_WAIT: return "server-start-wait";
        case ESP_AT_SERVICE_STATE_LISTENING: return "listening";
        case ESP_AT_SERVICE_STATE_BACKOFF: return "backoff";
        default: return "unknown";
    }
}

static app_status_t esp_at_service_line_handler (char const * p_line,
                                                 uint32_t line_bytes,
                                                 void * p_context)
{
    uint8_t link_id;

    (void) p_context;
    if ((NULL == p_line) || (0U == line_bytes))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (esp_at_service_line_equals(p_line, line_bytes, "ready"))
    {
        if (ESP_AT_SERVICE_STATE_RESET_BOOT_WAIT == g_diagnostics.state)
        {
            return APP_STATUS_OK;
        }
        esp_at_service_enter_recovery(APP_STATUS_IO);
        return APP_STATUS_OK;
    }
    if (esp_at_service_line_equals(p_line, line_bytes, "WIFI CONNECTED"))
    {
        g_diagnostics.wifi_connected = true;
        return APP_STATUS_OK;
    }
    if (esp_at_service_line_equals(p_line, line_bytes, "WIFI GOT IP"))
    {
        g_diagnostics.got_ip = true;
        if (ESP_AT_SERVICE_STATE_IP_QUERY_RETRY_WAIT == g_diagnostics.state)
        {
            esp_at_service_transition(ESP_AT_SERVICE_STATE_IP_QUERY_SEND, 0U);
        }
        return APP_STATUS_OK;
    }
    if (esp_at_service_line_equals(p_line, line_bytes, "WIFI DISCONNECT"))
    {
        g_diagnostics.wifi_disconnects++;
        g_diagnostics.wifi_connected = false;
        g_diagnostics.got_ip = false;
        if (ESP_AT_SERVICE_STATE_JOIN_WAIT != g_diagnostics.state)
        {
            esp_at_service_enter_recovery(APP_STATUS_UNAVAILABLE);
        }
        return APP_STATUS_OK;
    }
    if (esp_at_service_parse_link_event(p_line, line_bytes, ",CONNECT", &link_id))
    {
        app_status_t const status = lan_session_router_open_link(link_id);

        if (APP_STATUS_OK == status)
        {
            g_diagnostics.links_opened++;
        }
        else
        {
            g_diagnostics.links_rejected++;
            g_pending_close_mask |= (uint8_t) (1U << link_id);
        }
        return APP_STATUS_OK;
    }
    if (esp_at_service_parse_link_event(p_line, line_bytes, ",CLOSED", &link_id))
    {
        lan_session_router_close_link(link_id);
        g_pending_close_mask &= (uint8_t) ~(1U << link_id);
        g_diagnostics.links_closed++;
        if (g_send_link == link_id)
        {
            esp_at_service_finish_outbound(false);
        }
        if ((ESP_AT_IO_CLOSE_WAIT == g_io_state) && (g_close_link == link_id))
        {
            g_io_state = ESP_AT_IO_IDLE;
            g_close_link = ESP_AT_SERVICE_NO_LINK;
        }
        return APP_STATUS_OK;
    }
    if (esp_at_service_line_equals(p_line, line_bytes, ">") &&
        (ESP_AT_IO_SEND_WAIT_PROMPT == g_io_state))
    {
        return esp_at_service_send_outbound_payload();
    }
    if (esp_at_service_line_equals(p_line, line_bytes, "SEND OK") &&
        (ESP_AT_IO_SEND_WAIT_RESULT == g_io_state))
    {
        esp_at_service_finish_outbound(true);
        return APP_STATUS_OK;
    }
    if ((esp_at_service_line_equals(p_line, line_bytes, "SEND FAIL") ||
         esp_at_service_line_equals(p_line, line_bytes, "ERROR") ||
         esp_at_service_line_ends_with(p_line, line_bytes, ",CLOSED")) &&
        ((ESP_AT_IO_SEND_WAIT_PROMPT == g_io_state) ||
         (ESP_AT_IO_SEND_WAIT_RESULT == g_io_state)))
    {
        uint8_t const failed_link = g_send_link;

        esp_at_service_finish_outbound(false);
        esp_at_service_request_link_close(failed_link);
        return APP_STATUS_OK;
    }
    if ((ESP_AT_IO_CLOSE_WAIT == g_io_state) &&
        (esp_at_service_line_equals(p_line, line_bytes, "OK") ||
         esp_at_service_line_equals(p_line, line_bytes, "ERROR")))
    {
        g_io_state = ESP_AT_IO_IDLE;
        if (ESP_AT_SERVICE_NO_LINK != g_close_link)
        {
            g_pending_close_mask &= (uint8_t) ~(1U << g_close_link);
        }
        g_close_link = ESP_AT_SERVICE_NO_LINK;
        return APP_STATUS_OK;
    }
    if (ESP_AT_SERVICE_STATE_VERSION_WAIT == g_diagnostics.state)
    {
        esp_at_service_capture_version(p_line, line_bytes);
    }
    if (ESP_AT_SERVICE_STATE_IP_QUERY_WAIT == g_diagnostics.state)
    {
        esp_at_service_capture_station_ip(p_line, line_bytes);
    }
    if (esp_at_service_line_equals(p_line, line_bytes, "OK"))
    {
        if (esp_at_service_waiting_for_response(g_diagnostics.state) &&
            !g_command_response_armed)
        {
            return APP_STATUS_OK;
        }
        switch (g_diagnostics.state)
        {
            case ESP_AT_SERVICE_STATE_PROBE_WAIT:
                esp_at_service_transition(ESP_AT_SERVICE_STATE_ECHO_SEND, 0U);
                break;
            case ESP_AT_SERVICE_STATE_ECHO_WAIT:
                esp_at_service_transition(ESP_AT_SERVICE_STATE_VERSION_SEND, 0U);
                break;
            case ESP_AT_SERVICE_STATE_VERSION_WAIT:
                esp_at_service_transition(g_diagnostics.credentials_configured ?
                                          ESP_AT_SERVICE_STATE_WIFI_MODE_SEND :
                                          ESP_AT_SERVICE_STATE_CREDENTIALS_WAIT,
                                          0U);
                break;
            case ESP_AT_SERVICE_STATE_WIFI_MODE_WAIT:
                esp_at_service_transition(ESP_AT_SERVICE_STATE_JOIN_SEND, 0U);
                break;
            case ESP_AT_SERVICE_STATE_JOIN_WAIT:
                g_diagnostics.wifi_connected = true;
                g_ip_query_attempts = 0U;
                esp_at_service_transition(ESP_AT_SERVICE_STATE_IP_QUERY_SEND, 0U);
                break;
            case ESP_AT_SERVICE_STATE_IP_QUERY_WAIT:
                if ('\0' == g_diagnostics.station_ip[0])
                {
                    if (g_ip_query_attempts < ESP_AT_SERVICE_IP_QUERY_ATTEMPTS)
                    {
                        g_diagnostics.ip_query_retries++;
                        esp_at_service_transition(
                            ESP_AT_SERVICE_STATE_IP_QUERY_RETRY_WAIT,
                            ESP_AT_SERVICE_IP_RETRY_WAIT_US);
                    }
                    else
                    {
                        g_diagnostics.command_errors++;
                        esp_at_service_enter_recovery(APP_STATUS_DATA);
                    }
                }
                else
                {
                    g_diagnostics.got_ip = true;
                    esp_at_service_transition(ESP_AT_SERVICE_STATE_MUX_SEND, 0U);
                }
                break;
            case ESP_AT_SERVICE_STATE_MUX_WAIT:
                esp_at_service_transition(ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_SEND, 0U);
                break;
            case ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_WAIT:
                esp_at_service_transition(ESP_AT_SERVICE_STATE_SERVER_STOP_SEND, 0U);
                break;
            case ESP_AT_SERVICE_STATE_SERVER_STOP_WAIT:
                esp_at_service_transition(ESP_AT_SERVICE_STATE_SERVER_START_SEND, 0U);
                break;
            case ESP_AT_SERVICE_STATE_SERVER_START_WAIT:
                g_diagnostics.server_listening = true;
                g_diagnostics.consecutive_failures = 0U;
                g_recoveries_since_reset = 0U;
                g_backoff_us = ESP_AT_SERVICE_BACKOFF_MIN_US;
                esp_at_service_transition(ESP_AT_SERVICE_STATE_LISTENING, 0U);
                break;
            default:
                break;
        }
        return APP_STATUS_OK;
    }
    if (esp_at_service_line_equals(p_line, line_bytes, "ERROR") ||
        esp_at_service_line_equals(p_line, line_bytes, "FAIL") ||
        esp_at_service_line_ends_with(p_line, line_bytes, " FAIL") ||
        ((line_bytes >= 4U) && (0 == memcmp(p_line, "busy", 4U))))
    {
        if (esp_at_service_waiting_for_response(g_diagnostics.state) &&
            !g_command_response_armed)
        {
            return APP_STATUS_OK;
        }
        if (ESP_AT_SERVICE_STATE_SERVER_STOP_WAIT == g_diagnostics.state)
        {
            esp_at_service_transition(ESP_AT_SERVICE_STATE_SERVER_START_SEND, 0U);
        }
        else
        {
            g_diagnostics.command_errors++;
            esp_at_service_enter_recovery(APP_STATUS_IO);
        }
    }
    return APP_STATUS_OK;
}

static app_status_t esp_at_service_ipd_handler (uint8_t link_id,
                                                uint8_t const * p_data,
                                                uint32_t data_bytes,
                                                void * p_context)
{
    app_status_t status;

    (void) p_context;
    status = lan_session_router_feed(link_id, p_data, data_bytes);
    if (APP_STATUS_OK == status)
    {
        g_diagnostics.ipd_bytes += data_bytes;
    }
    else if ((APP_STATUS_PROTOCOL == status) || (APP_STATUS_DATA == status) ||
             (APP_STATUS_BUSY == status))
    {
        esp_at_service_request_link_close(link_id);
        return APP_STATUS_OK;
    }
    return status;
}

static app_status_t esp_at_service_send_text (char const * p_text)
{
    app_status_t const status = esp_at_transport_write(
        (uint8_t const *) p_text, strlen(p_text));

    if (APP_STATUS_OK == status)
    {
        esp_at_transport_diagnostics_t transport;

        esp_at_transport_get_diagnostics(&transport);
        g_command_tx_completion_target = transport.tx_requests;
        g_command_response_armed = false;
        g_diagnostics.commands_sent++;
    }
    return status;
}

static app_status_t esp_at_service_send_join (void)
{
    char command[ESP_AT_SERVICE_COMMAND_BYTES];
    size_t used = 0U;

    if (!g_diagnostics.credentials_configured)
    {
        return APP_STATUS_UNAVAILABLE;
    }
    (void) memcpy(command, "AT+CWJAP=\"", sizeof("AT+CWJAP=\"") - 1U);
    used = sizeof("AT+CWJAP=\"") - 1U;
    if (!esp_at_service_append_escaped(command, sizeof(command), &used,
                                       ESP_AT_WIFI_SSID) ||
        ((used + 3U) >= sizeof(command)))
    {
        return APP_STATUS_DATA;
    }
    command[used++] = '"';
    command[used++] = ',';
    command[used++] = '"';
    if (!esp_at_service_append_escaped(command, sizeof(command), &used,
                                       ESP_AT_WIFI_PASSWORD) ||
        ((used + 3U) >= sizeof(command)))
    {
        return APP_STATUS_DATA;
    }
    command[used++] = '"';
    command[used++] = '\r';
    command[used++] = '\n';
    command[used] = '\0';
    return esp_at_service_send_text(command);
}

static app_status_t esp_at_service_start_outbound (void)
{
    lan_session_outbound_t outbound;
    char command[48];
    int command_bytes;
    app_status_t status = lan_session_router_peek_outbound(&outbound);

    if (APP_STATUS_OK != status)
    {
        return status;
    }
    command_bytes = snprintf(command,
                             sizeof(command),
                             "AT+CIPSEND=%u,%lu\r\n",
                             (unsigned int) outbound.link_id,
                             (unsigned long) outbound.data_bytes);
    if ((command_bytes < 0) || ((size_t) command_bytes >= sizeof(command)))
    {
        return APP_STATUS_DATA;
    }
    status = esp_at_transport_write((uint8_t const *) command,
                                    (size_t) command_bytes);
    if (APP_STATUS_OK != status)
    {
        return status;
    }
    status = lan_session_router_mark_outbound_in_flight(outbound.link_id,
                                                        outbound.control_response);
    if (APP_STATUS_OK != status)
    {
        return status;
    }
    g_send_link = outbound.link_id;
    g_send_control_response = outbound.control_response;
    g_send_buffer_locked = true;
    g_send_data = outbound.p_data;
    g_send_bytes = outbound.data_bytes;
    g_io_state = ESP_AT_IO_SEND_WAIT_PROMPT;
    g_deadline_us = g_now_us + ESP_AT_SERVICE_SEND_TIMEOUT_US;
    g_diagnostics.commands_sent++;
    g_diagnostics.send_requests++;
    return APP_STATUS_OK;
}

static app_status_t esp_at_service_send_outbound_payload (void)
{
    app_status_t status;

    if (!g_send_buffer_locked || (NULL == g_send_data) || (0U == g_send_bytes))
    {
        esp_at_service_finish_outbound(false);
        return APP_STATUS_DATA;
    }
    status = esp_at_transport_write(g_send_data, g_send_bytes);
    if (APP_STATUS_OK == status)
    {
        g_io_state = ESP_AT_IO_SEND_WAIT_RESULT;
        g_deadline_us = g_now_us + ESP_AT_SERVICE_SEND_TIMEOUT_US;
    }
    else
    {
        esp_at_service_finish_outbound(false);
    }
    return status;
}

static app_status_t esp_at_service_send_pending_close (void)
{
    uint8_t const link_id = esp_at_service_next_pending_close();
    char command[32];
    int command_bytes = snprintf(command,
                                 sizeof(command),
                                 "AT+CIPCLOSE=%u\r\n",
                                 (unsigned int) link_id);
    app_status_t status;

    if ((command_bytes < 0) || ((size_t) command_bytes >= sizeof(command)))
    {
        return APP_STATUS_DATA;
    }
    status = esp_at_transport_write((uint8_t const *) command,
                                    (size_t) command_bytes);
    if (APP_STATUS_OK == status)
    {
        g_io_state = ESP_AT_IO_CLOSE_WAIT;
        g_close_link = link_id;
        g_deadline_us = g_now_us + ESP_AT_SERVICE_COMMAND_TIMEOUT_US;
        g_diagnostics.commands_sent++;
    }
    return status;
}

static void esp_at_service_process_listening_io (void)
{
    uint8_t link_id;

    if (ESP_AT_IO_IDLE != g_io_state)
    {
        return;
    }
    for (link_id = 0U; link_id <= ESP_AT_SERVICE_MAX_LINK_ID; link_id++)
    {
        if (lan_session_router_should_close(link_id))
        {
            esp_at_service_request_link_close(link_id);
        }
    }
    if (0U != g_pending_close_mask)
    {
        (void) esp_at_service_send_pending_close();
    }
    else
    {
        (void) esp_at_service_start_outbound();
    }
}

static void esp_at_service_check_timeouts (uint64_t now_us)
{
    if ((ESP_AT_IO_IDLE != g_io_state) && (now_us >= g_deadline_us))
    {
        g_diagnostics.command_timeouts++;
        if ((ESP_AT_IO_SEND_WAIT_PROMPT == g_io_state) ||
            (ESP_AT_IO_SEND_WAIT_RESULT == g_io_state))
        {
            esp_at_service_finish_outbound(false);
        }
        g_io_state = ESP_AT_IO_IDLE;
        esp_at_service_enter_recovery(APP_STATUS_TIMEOUT);
    }
    if (esp_at_service_waiting_for_response(g_diagnostics.state) &&
        (0U < g_deadline_us) && (now_us >= g_deadline_us))
    {
        g_diagnostics.command_timeouts++;
        esp_at_service_enter_recovery(APP_STATUS_TIMEOUT);
    }
}

static uint8_t esp_at_service_next_pending_close (void)
{
    uint8_t link_id;

    for (link_id = 0U; link_id <= ESP_AT_SERVICE_MAX_LINK_ID; link_id++)
    {
        if (0U != (g_pending_close_mask & (uint8_t) (1U << link_id)))
        {
            return link_id;
        }
    }
    return ESP_AT_SERVICE_NO_LINK;
}

static void esp_at_service_transition (esp_at_service_state_t state,
                                       uint64_t timeout_us)
{
    if (g_diagnostics.state != state)
    {
        g_diagnostics.state_transitions++;
    }
    g_diagnostics.state = state;
    g_deadline_us = (0U == timeout_us) ? 0U : g_now_us + timeout_us;
    g_diagnostics.network_state = esp_at_service_network_state();
}

static void esp_at_service_enter_recovery (app_status_t status)
{
    uint64_t delay_us = g_backoff_us;

    if ((ESP_AT_SERVICE_STATE_BACKOFF == g_diagnostics.state) ||
        (ESP_AT_SERVICE_STATE_RESET_FAULT == g_diagnostics.state))
    {
        return;
    }
    esp_at_service_finish_outbound(false);
    esp_at_service_close_all_links();
    g_io_state = ESP_AT_IO_IDLE;
    g_command_response_armed = false;
    g_pending_close_mask = 0U;
    g_close_link = ESP_AT_SERVICE_NO_LINK;
    g_diagnostics.server_listening = false;
    g_diagnostics.wifi_connected = false;
    g_diagnostics.got_ip = false;
    g_diagnostics.station_ip[0] = '\0';
    g_diagnostics.recoveries++;
    g_diagnostics.consecutive_failures++;
    g_recoveries_since_reset++;
    if (g_recoveries_since_reset >= ESP_AT_SERVICE_RESET_THRESHOLD)
    {
        g_reset_after_backoff = true;
    }
    g_diagnostics.last_status = status;
    esp_at_service_transition(ESP_AT_SERVICE_STATE_BACKOFF, delay_us);
    if (g_backoff_us < ESP_AT_SERVICE_BACKOFF_MAX_US)
    {
        g_backoff_us *= 2U;
        if (g_backoff_us > ESP_AT_SERVICE_BACKOFF_MAX_US)
        {
            g_backoff_us = ESP_AT_SERVICE_BACKOFF_MAX_US;
        }
    }
}

static app_status_t esp_at_service_set_reset (bool asserted)
{
    app_status_t const status = esp_at_reset_set_asserted(asserted);

    if (APP_STATUS_OK == status)
    {
        g_diagnostics.reset_asserted = asserted;
    }
    else
    {
        g_diagnostics.reset_errors++;
        g_diagnostics.last_status = status;
    }
    return status;
}

static void esp_at_service_finish_outbound (bool sent)
{
    if (g_send_buffer_locked)
    {
        lan_session_router_consume_outbound(g_send_link,
                                            g_send_control_response,
                                            sent);
        if (sent)
        {
            g_diagnostics.send_completions++;
        }
        else
        {
            g_diagnostics.send_failures++;
        }
    }
    g_send_buffer_locked = false;
    g_send_control_response = false;
    g_send_link = ESP_AT_SERVICE_NO_LINK;
    g_send_data = NULL;
    g_send_bytes = 0U;
    if ((ESP_AT_IO_SEND_WAIT_PROMPT == g_io_state) ||
        (ESP_AT_IO_SEND_WAIT_RESULT == g_io_state))
    {
        g_io_state = ESP_AT_IO_IDLE;
    }
}

static void esp_at_service_close_all_links (void)
{
    uint8_t link_id;

    for (link_id = 0U; link_id <= ESP_AT_SERVICE_MAX_LINK_ID; link_id++)
    {
        lan_session_router_close_link(link_id);
    }
}

static void esp_at_service_request_link_close (uint8_t link_id)
{
    if (link_id <= ESP_AT_SERVICE_MAX_LINK_ID)
    {
        lan_session_router_close_link(link_id);
        g_pending_close_mask |= (uint8_t) (1U << link_id);
    }
}

static bool esp_at_service_line_equals (char const * p_line,
                                        uint32_t line_bytes,
                                        char const * p_expected)
{
    size_t const expected_bytes = strlen(p_expected);

    return ((size_t) line_bytes == expected_bytes) &&
           (0 == memcmp(p_line, p_expected, expected_bytes));
}

static bool esp_at_service_line_ends_with (char const * p_line,
                                           uint32_t line_bytes,
                                           char const * p_suffix)
{
    size_t const suffix_bytes = strlen(p_suffix);

    return ((size_t) line_bytes >= suffix_bytes) &&
           (0 == memcmp(&p_line[line_bytes - (uint32_t) suffix_bytes],
                        p_suffix,
                        suffix_bytes));
}

static bool esp_at_service_parse_link_event (char const * p_line,
                                             uint32_t line_bytes,
                                             char const * p_suffix,
                                             uint8_t * p_link_id)
{
    size_t const suffix_bytes = strlen(p_suffix);
    uint8_t const link_character = (uint8_t) p_line[0];

    if ((NULL == p_link_id) || ((size_t) line_bytes != (suffix_bytes + 1U)) ||
        (link_character < (uint8_t) '0') ||
        (link_character > (uint8_t) ('0' + ESP_AT_SERVICE_MAX_LINK_ID)) ||
        (0 != memcmp(&p_line[1], p_suffix, suffix_bytes)))
    {
        return false;
    }
    *p_link_id = (uint8_t) (link_character - (uint8_t) '0');
    return true;
}

static bool esp_at_service_append_escaped (char * p_output,
                                           size_t output_bytes,
                                           size_t * p_used,
                                           char const * p_text)
{
    size_t index;

    if ((NULL == p_output) || (NULL == p_used) || (NULL == p_text))
    {
        return false;
    }
    for (index = 0U; '\0' != p_text[index]; index++)
    {
        char const value = p_text[index];
        bool const escape = ('\\' == value) || ('"' == value) || (',' == value);

        if (('\r' == value) || ('\n' == value) ||
            (*p_used + (escape ? 2U : 1U) >= output_bytes))
        {
            return false;
        }
        if (escape)
        {
            p_output[(*p_used)++] = '\\';
        }
        p_output[(*p_used)++] = value;
    }
    p_output[*p_used] = '\0';
    return true;
}

static void esp_at_service_capture_version (char const * p_line,
                                            uint32_t line_bytes)
{
    static char const prefix[] = "AT version:";
    uint32_t copy_bytes;

    if ((0U != g_diagnostics.firmware_version[0]) ||
        (line_bytes < (sizeof(prefix) - 1U)) ||
        (0 != memcmp(p_line, prefix, sizeof(prefix) - 1U)))
    {
        return;
    }
    copy_bytes = line_bytes;
    if (copy_bytes >= sizeof(g_diagnostics.firmware_version))
    {
        copy_bytes = sizeof(g_diagnostics.firmware_version) - 1U;
    }
    (void) memcpy(g_diagnostics.firmware_version, p_line, copy_bytes);
    g_diagnostics.firmware_version[copy_bytes] = '\0';
}

static void esp_at_service_capture_station_ip (char const * p_line,
                                               uint32_t line_bytes)
{
    static char const prefix[] = "+CIFSR:STAIP,\"";
    uint32_t const prefix_bytes = (uint32_t) (sizeof(prefix) - 1U);
    uint32_t address_bytes;

    if ((NULL == p_line) || (line_bytes <= (prefix_bytes + 1U)) ||
        (0 != memcmp(p_line, prefix, prefix_bytes)) ||
        ('"' != p_line[line_bytes - 1U]))
    {
        return;
    }
    address_bytes = line_bytes - prefix_bytes - 1U;
    if ((address_bytes >= sizeof(g_diagnostics.station_ip)) ||
        !esp_at_service_parse_ipv4(&p_line[prefix_bytes], address_bytes))
    {
        return;
    }
    (void) memcpy(g_diagnostics.station_ip,
                  &p_line[prefix_bytes],
                  address_bytes);
    g_diagnostics.station_ip[address_bytes] = '\0';
}

static bool esp_at_service_parse_ipv4 (char const * p_text,
                                       uint32_t text_bytes)
{
    uint32_t index = 0U;
    uint32_t octet_count = 0U;
    bool any_nonzero = false;

    if ((NULL == p_text) || (0U == text_bytes))
    {
        return false;
    }
    while (index < text_bytes)
    {
        uint32_t value = 0U;
        uint32_t digits = 0U;

        while ((index < text_bytes) && (p_text[index] >= '0') &&
               (p_text[index] <= '9'))
        {
            value = value * 10U + (uint32_t) (p_text[index] - '0');
            digits++;
            index++;
            if ((digits > 3U) || (value > 255U))
            {
                return false;
            }
        }
        if (0U == digits)
        {
            return false;
        }
        any_nonzero = any_nonzero || (0U != value);
        octet_count++;
        if (index == text_bytes)
        {
            break;
        }
        if (('.' != p_text[index]) || (octet_count >= 4U))
        {
            return false;
        }
        index++;
    }

    return (4U == octet_count) && any_nonzero;
}

static bool esp_at_service_waiting_for_response (esp_at_service_state_t state)
{
    return (ESP_AT_SERVICE_STATE_PROBE_WAIT == state) ||
           (ESP_AT_SERVICE_STATE_ECHO_WAIT == state) ||
           (ESP_AT_SERVICE_STATE_VERSION_WAIT == state) ||
           (ESP_AT_SERVICE_STATE_WIFI_MODE_WAIT == state) ||
           (ESP_AT_SERVICE_STATE_JOIN_WAIT == state) ||
           (ESP_AT_SERVICE_STATE_IP_QUERY_WAIT == state) ||
           (ESP_AT_SERVICE_STATE_MUX_WAIT == state) ||
           (ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_WAIT == state) ||
           (ESP_AT_SERVICE_STATE_SERVER_STOP_WAIT == state) ||
           (ESP_AT_SERVICE_STATE_SERVER_START_WAIT == state);
}

static esp_at_network_state_t esp_at_service_network_state (void)
{
    lan_session_router_diagnostics_t session;

    if (!g_diagnostics.initialized)
    {
        return ESP_AT_NETWORK_OFFLINE;
    }
    lan_session_router_get_diagnostics(&session);
    if (0U < session.open_links)
    {
        return ESP_AT_NETWORK_SESSION;
    }
    switch (g_diagnostics.state)
    {
        case ESP_AT_SERVICE_STATE_RESET_ASSERT:
        case ESP_AT_SERVICE_STATE_RESET_HOLD:
        case ESP_AT_SERVICE_STATE_RESET_RELEASE:
        case ESP_AT_SERVICE_STATE_RESET_BOOT_WAIT:
            return ESP_AT_NETWORK_PROBING;
        case ESP_AT_SERVICE_STATE_RESET_FAULT:
            return ESP_AT_NETWORK_OFFLINE;
        case ESP_AT_SERVICE_STATE_CREDENTIALS_WAIT:
            return ESP_AT_NETWORK_CREDENTIALS_WAIT;
        case ESP_AT_SERVICE_STATE_WIFI_MODE_SEND:
        case ESP_AT_SERVICE_STATE_WIFI_MODE_WAIT:
        case ESP_AT_SERVICE_STATE_JOIN_SEND:
        case ESP_AT_SERVICE_STATE_JOIN_WAIT:
        case ESP_AT_SERVICE_STATE_IP_QUERY_SEND:
        case ESP_AT_SERVICE_STATE_IP_QUERY_WAIT:
        case ESP_AT_SERVICE_STATE_IP_QUERY_RETRY_WAIT:
        case ESP_AT_SERVICE_STATE_MUX_SEND:
        case ESP_AT_SERVICE_STATE_MUX_WAIT:
        case ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_SEND:
        case ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_WAIT:
        case ESP_AT_SERVICE_STATE_SERVER_STOP_SEND:
        case ESP_AT_SERVICE_STATE_SERVER_STOP_WAIT:
        case ESP_AT_SERVICE_STATE_SERVER_START_SEND:
        case ESP_AT_SERVICE_STATE_SERVER_START_WAIT:
            return ESP_AT_NETWORK_JOINING;
        case ESP_AT_SERVICE_STATE_LISTENING:
            return ESP_AT_NETWORK_LISTENING;
        case ESP_AT_SERVICE_STATE_BACKOFF:
            return ESP_AT_NETWORK_BACKOFF;
        case ESP_AT_SERVICE_STATE_CLOSED:
            return ESP_AT_NETWORK_OFFLINE;
        default:
            return ESP_AT_NETWORK_PROBING;
    }
}

static app_status_t esp_at_service_record (app_status_t status)
{
    g_diagnostics.last_status = status;
    return status;
}

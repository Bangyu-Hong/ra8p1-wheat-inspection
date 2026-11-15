#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_at_service.h"
#include "esp_at_reset.h"
#include "ra8p_wire_contract.h"
#include "wire_protocol.h"

#define TEST_CAPTURE_BYTES  (32768U)

#if defined(ESP_AT_CREDENTIALS_HOST_TEST)
 #define TEST_CREDENTIALS_CONFIGURED  (1)
#else
 #define TEST_CREDENTIALS_CONFIGURED  (0)
#endif

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

static uint8_t const * g_mock_active_source;
static uint32_t g_mock_active_bytes;
static uint8_t g_mock_transmitted[TEST_CAPTURE_BYTES];
static uint32_t g_mock_transmitted_bytes;
static bool g_mock_reset_levels[16];
static uint32_t g_mock_reset_calls;
static uint32_t g_mock_reset_fail_on_call;

static fsp_err_t mock_open(uart_ctrl_t * const p_ctrl, uart_cfg_t const * const p_cfg);
static fsp_err_t mock_write(uart_ctrl_t * const p_ctrl,
                            uint8_t const * const p_src,
                            uint32_t const bytes);
static fsp_err_t mock_close(uart_ctrl_t * const p_ctrl);
static fsp_err_t mock_abort(uart_ctrl_t * const p_ctrl, uart_dir_t direction);
static void mock_reset(void);
static void mock_complete(void);
static void inject_rx(uint8_t const * p_data, uint32_t data_bytes);
static int service_advance_initial_reset(void);
static int service_process(uint64_t now_us);
static int service_send_state(uint64_t now_us,
                              esp_at_service_state_t send_state,
                              char const * p_expected_command);
static int service_reply(uint64_t now_us,
                         char const * p_reply,
                         esp_at_service_state_t expected_state);
static int test_probe_and_credentials_gate(void);
static int test_reset_gpio_failure(void);
static int test_recovery_reset_is_bounded(void);
static int test_rx_overflow_enters_recovery(void);
#if TEST_CREDENTIALS_CONFIGURED
static bool capture_contains(uint8_t const * p_expected, uint32_t expected_bytes);
static int service_advance_to_join_wait(uint64_t * p_now_us);
static int service_advance_to_server_stop_wait(uint64_t * p_now_us);
static int test_join_failure_backoff_reaches_reset(void);
static int test_ip_query_retries_until_dhcp_ready(void);
static int test_ip_query_retry_limit_enters_recovery(void);
static int test_join_ignores_transient_disconnect_event(void);
static int test_server_start_response_requires_completed_tx(void);
static int test_join_server_and_tcp_session(void);
#endif

static uart_api_t const g_mock_uart_api =
{
    .open = mock_open,
    .write = mock_write,
    .close = mock_close,
    .communicationAbort = mock_abort,
};

static uint32_t g_mock_uart_ctrl;
static uart_cfg_t const g_mock_uart_cfg = {0};

const uart_instance_t g_uart0 =
{
    .p_ctrl = (uart_ctrl_t *) &g_mock_uart_ctrl,
    .p_cfg = &g_mock_uart_cfg,
    .p_api = &g_mock_uart_api,
};

app_status_t esp_at_reset_set_asserted (bool asserted)
{
    uint32_t const call = g_mock_reset_calls + 1U;

    if (g_mock_reset_calls < (uint32_t) (sizeof(g_mock_reset_levels) /
                                         sizeof(g_mock_reset_levels[0])))
    {
        g_mock_reset_levels[g_mock_reset_calls] = asserted;
    }
    g_mock_reset_calls = call;
    if ((0U != g_mock_reset_fail_on_call) &&
        (call == g_mock_reset_fail_on_call))
    {
        return APP_STATUS_IO;
    }
    return APP_STATUS_OK;
}

int main (void)
{
    TEST_CHECK(0 == test_reset_gpio_failure());
    TEST_CHECK(0 == test_recovery_reset_is_bounded());
    TEST_CHECK(0 == test_rx_overflow_enters_recovery());
    TEST_CHECK(0 == test_probe_and_credentials_gate());
#if TEST_CREDENTIALS_CONFIGURED
    TEST_CHECK(0 == test_join_failure_backoff_reaches_reset());
    TEST_CHECK(0 == test_ip_query_retries_until_dhcp_ready());
    TEST_CHECK(0 == test_ip_query_retry_limit_enters_recovery());
    TEST_CHECK(0 == test_join_ignores_transient_disconnect_event());
    TEST_CHECK(0 == test_server_start_response_requires_completed_tx());
    TEST_CHECK(0 == test_join_server_and_tcp_session());
    (void) printf("esp-at-service status=ok probe=ok credentials=test join=ok "
                  "tcp=hello-snapshot-telemetry recovery=bounded-backoff-reset\n");
#else
    (void) printf("esp-at-service status=ok probe=ok credentials=wait-safe\n");
#endif
    return 0;
}

static fsp_err_t mock_open (uart_ctrl_t * const p_ctrl, uart_cfg_t const * const p_cfg)
{
    return ((NULL != p_ctrl) && (NULL != p_cfg)) ? FSP_SUCCESS : FSP_ERR_INTERNAL;
}

static fsp_err_t mock_write (uart_ctrl_t * const p_ctrl,
                             uint8_t const * const p_src,
                             uint32_t const bytes)
{
    if ((NULL == p_ctrl) || (NULL == p_src) || (0U == bytes) ||
        (NULL != g_mock_active_source))
    {
        return FSP_ERR_IN_USE;
    }
    g_mock_active_source = p_src;
    g_mock_active_bytes = bytes;
    return FSP_SUCCESS;
}

static fsp_err_t mock_close (uart_ctrl_t * const p_ctrl)
{
    return (NULL != p_ctrl) ? FSP_SUCCESS : FSP_ERR_INTERNAL;
}

static fsp_err_t mock_abort (uart_ctrl_t * const p_ctrl, uart_dir_t direction)
{
    if ((NULL == p_ctrl) || (UART_DIR_TX != direction))
    {
        return FSP_ERR_INTERNAL;
    }
    g_mock_active_source = NULL;
    g_mock_active_bytes = 0U;
    return FSP_SUCCESS;
}

static void mock_reset (void)
{
    g_mock_reset_fail_on_call = 0U;
    (void) esp_at_service_close();
    g_mock_active_source = NULL;
    g_mock_active_bytes = 0U;
    g_mock_transmitted_bytes = 0U;
    g_mock_reset_calls = 0U;
    g_mock_reset_fail_on_call = 0U;
    (void) memset(g_mock_transmitted, 0, sizeof(g_mock_transmitted));
    (void) memset(g_mock_reset_levels, 0, sizeof(g_mock_reset_levels));
}

static void mock_complete (void)
{
    uart_callback_args_t args = {.channel = 0U, .event = UART_EVENT_TX_COMPLETE};

    if ((NULL == g_mock_active_source) ||
        (g_mock_active_bytes > (TEST_CAPTURE_BYTES - g_mock_transmitted_bytes)))
    {
        return;
    }
    (void) memcpy(&g_mock_transmitted[g_mock_transmitted_bytes],
                  g_mock_active_source,
                  g_mock_active_bytes);
    g_mock_transmitted_bytes += g_mock_active_bytes;
    g_mock_active_source = NULL;
    g_mock_active_bytes = 0U;
    esp_at_uart0_callback(&args);
}

static void inject_rx (uint8_t const * p_data, uint32_t data_bytes)
{
    uart_callback_args_t args = {.channel = 0U, .event = UART_EVENT_RX_CHAR};
    uint32_t index;

    for (index = 0U; index < data_bytes; index++)
    {
        args.data = p_data[index];
        esp_at_uart0_callback(&args);
    }
}

static int service_process (uint64_t now_us)
{
    app_status_t const status = esp_at_service_process(now_us);

    TEST_CHECK((APP_STATUS_OK == status) || (APP_STATUS_BUSY == status));
    return 0;
}

static int service_advance_initial_reset (void)
{
    esp_at_service_diagnostics_t diagnostics;

    TEST_CHECK(0 == service_process(0U));
    TEST_CHECK(1U == g_mock_reset_calls);
    TEST_CHECK(g_mock_reset_levels[0]);
    TEST_CHECK(0U == g_mock_transmitted_bytes);
    TEST_CHECK(0 == service_process(199999U));
    TEST_CHECK(1U == g_mock_reset_calls);
    TEST_CHECK(0 == service_process(200000U));
    TEST_CHECK(1U == g_mock_reset_calls);
    TEST_CHECK(0 == service_process(200001U));
    TEST_CHECK(2U == g_mock_reset_calls);
    TEST_CHECK(!g_mock_reset_levels[1]);
    TEST_CHECK(0 == service_process(2000000U));
    TEST_CHECK(0U == g_mock_transmitted_bytes);
    TEST_CHECK(0 == service_process(2000001U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_PROBE_SEND == diagnostics.state);
    TEST_CHECK(1U == diagnostics.reset_count);
    TEST_CHECK(0U == diagnostics.reset_errors);
    TEST_CHECK(!diagnostics.reset_asserted);
    return 0;
}

static int test_reset_gpio_failure (void)
{
    esp_at_service_diagnostics_t diagnostics;

    mock_reset();
    g_mock_reset_fail_on_call = 1U;
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(APP_STATUS_IO == esp_at_service_process(0U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_RESET_FAULT == diagnostics.state);
    TEST_CHECK(1U == diagnostics.reset_errors);
    TEST_CHECK(0U == diagnostics.reset_count);
    TEST_CHECK(!diagnostics.reset_asserted);
    inject_rx((uint8_t const *) "\r\nready\r\n", 9U);
    TEST_CHECK(APP_STATUS_IO == esp_at_service_process(1000000U));
    TEST_CHECK(1U == g_mock_reset_calls);
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_RESET_FAULT == diagnostics.state);
    TEST_CHECK(0U == diagnostics.recoveries);
    g_mock_reset_fail_on_call = 0U;
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    TEST_CHECK(!g_mock_reset_levels[g_mock_reset_calls - 1U]);
    return 0;
}

static int test_recovery_reset_is_bounded (void)
{
    esp_at_service_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(0 == service_advance_initial_reset());
    TEST_CHECK(0 == service_send_state(2000002U,
                                       ESP_AT_SERVICE_STATE_PROBE_SEND,
                                       "AT\r\n"));

    TEST_CHECK(0 == service_process(4000002U));
    TEST_CHECK(0 == service_process(5000002U));
    TEST_CHECK(0 == service_send_state(5000003U,
                                       ESP_AT_SERVICE_STATE_PROBE_SEND,
                                       "AT\r\n"));
    TEST_CHECK(0 == service_process(7000003U));
    TEST_CHECK(0 == service_process(9000003U));
    TEST_CHECK(0 == service_send_state(9000004U,
                                       ESP_AT_SERVICE_STATE_PROBE_SEND,
                                       "AT\r\n"));
    TEST_CHECK(0 == service_process(11000004U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_BACKOFF == diagnostics.state);
    TEST_CHECK(3U == diagnostics.recoveries);
    TEST_CHECK(1U == diagnostics.reset_count);
    TEST_CHECK(2U == g_mock_reset_calls);

    TEST_CHECK(0 == service_process(15000004U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_RESET_ASSERT == diagnostics.state);
    TEST_CHECK(0 == service_process(15000005U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_RESET_HOLD == diagnostics.state);
    TEST_CHECK(2U == diagnostics.reset_count);
    TEST_CHECK(3U == g_mock_reset_calls);
    TEST_CHECK(g_mock_reset_levels[2]);
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    return 0;
}

static int test_rx_overflow_enters_recovery (void)
{
    static uint8_t overflow_data[ESP_AT_TRANSPORT_RX_BYTES + 1U];
    esp_at_service_diagnostics_t diagnostics;

    (void) memset(overflow_data, 'X', sizeof(overflow_data));
    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(0 == service_advance_initial_reset());
    inject_rx(overflow_data, sizeof(overflow_data));
    TEST_CHECK(0 == service_process(2000002U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_BACKOFF == diagnostics.state);
    TEST_CHECK(1U == diagnostics.recoveries);
    TEST_CHECK(0U < diagnostics.transport.rx_overflows);
    TEST_CHECK(0U == diagnostics.transport.rx_buffered_bytes);
    TEST_CHECK(0U < diagnostics.parser_errors);
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    return 0;
}

static int service_send_state (uint64_t now_us,
                               esp_at_service_state_t send_state,
                               char const * p_expected_command)
{
    esp_at_service_diagnostics_t diagnostics;
    uint32_t const before_bytes = g_mock_transmitted_bytes;

    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(send_state == diagnostics.state);
    TEST_CHECK(0 == service_process(now_us));
    TEST_CHECK(NULL != g_mock_active_source);
    mock_complete();
    TEST_CHECK(0 == service_process(now_us + 1U));
    TEST_CHECK((uint32_t) strlen(p_expected_command) ==
               (g_mock_transmitted_bytes - before_bytes));
    TEST_CHECK(0 == memcmp(&g_mock_transmitted[before_bytes],
                           p_expected_command,
                           strlen(p_expected_command)));
    return 0;
}

static int service_reply (uint64_t now_us,
                          char const * p_reply,
                          esp_at_service_state_t expected_state)
{
    esp_at_service_diagnostics_t diagnostics;

    inject_rx((uint8_t const *) p_reply, (uint32_t) strlen(p_reply));
    TEST_CHECK(0 == service_process(now_us));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(expected_state == diagnostics.state);
    return 0;
}

#if TEST_CREDENTIALS_CONFIGURED
static bool capture_contains (uint8_t const * p_expected, uint32_t expected_bytes)
{
    uint32_t offset;

    if ((NULL == p_expected) || (0U == expected_bytes) ||
        (expected_bytes > g_mock_transmitted_bytes))
    {
        return false;
    }
    for (offset = 0U; offset <= (g_mock_transmitted_bytes - expected_bytes); offset++)
    {
        if (0 == memcmp(&g_mock_transmitted[offset], p_expected, expected_bytes))
        {
            return true;
        }
    }
    return false;
}

static int service_advance_to_join_wait (uint64_t * p_now_us)
{
    uint64_t now_us;

    TEST_CHECK(NULL != p_now_us);
    now_us = *p_now_us;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_PROBE_SEND,
                                       "AT\r\n"));
    now_us += 10U;
    TEST_CHECK(0 == service_reply(now_us, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_ECHO_SEND));
    now_us += 10U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_ECHO_SEND,
                                       "ATE0\r\n"));
    now_us += 10U;
    TEST_CHECK(0 == service_reply(now_us, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_VERSION_SEND));
    now_us += 10U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_VERSION_SEND,
                                       "AT+GMR\r\n"));
    now_us += 10U;
    TEST_CHECK(0 == service_reply(now_us, "\r\nAT version:test\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_WIFI_MODE_SEND));
    now_us += 10U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_WIFI_MODE_SEND,
                                       "AT+CWMODE=1\r\n"));
    now_us += 10U;
    TEST_CHECK(0 == service_reply(now_us, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_JOIN_SEND));
    now_us += 10U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_JOIN_SEND,
                                       "AT+CWJAP=\"TestNet\",\"secret\"\r\n"));
    *p_now_us = now_us + 10U;
    return 0;
}

static int service_advance_to_server_stop_wait (uint64_t * p_now_us)
{
    uint64_t now_us;

    TEST_CHECK(NULL != p_now_us);
    now_us = *p_now_us;
    TEST_CHECK(0 == service_advance_to_join_wait(&now_us));
    TEST_CHECK(0 == service_reply(now_us,
                                  "\r\nWIFI CONNECTED\r\nWIFI GOT IP\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_IP_QUERY_SEND));
    now_us += 10U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_IP_QUERY_SEND,
                                       "AT+CIFSR\r\n"));
    now_us += 10U;
    TEST_CHECK(0 == service_reply(now_us,
                                  "\r\n+CIFSR:STAIP,\"192.168.10.26\"\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_MUX_SEND));
    now_us += 10U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_MUX_SEND,
                                       "AT+CIPMUX=1\r\n"));
    now_us += 10U;
    TEST_CHECK(0 == service_reply(now_us, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_SEND));
    now_us += 10U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_SEND,
                                       "AT+CIPSERVERMAXCONN=5\r\n"));
    now_us += 10U;
    TEST_CHECK(0 == service_reply(now_us, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_SERVER_STOP_SEND));
    now_us += 10U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_SERVER_STOP_SEND,
                                       "AT+CIPSERVER=0\r\n"));
    *p_now_us = now_us + 10U;
    return 0;
}

static int test_server_start_response_requires_completed_tx (void)
{
    esp_at_service_diagnostics_t diagnostics;
    uint64_t now_us = 2000002U;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(0 == service_advance_initial_reset());
    TEST_CHECK(0 == service_advance_to_server_stop_wait(&now_us));
    TEST_CHECK(0 == service_reply(now_us, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_SERVER_START_SEND));

    now_us += 10U;
    TEST_CHECK(0 == service_process(now_us));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_SERVER_START_WAIT == diagnostics.state);
    TEST_CHECK(10U == diagnostics.commands_sent);
    TEST_CHECK(NULL != g_mock_active_source);

    inject_rx((uint8_t const *) "\r\nOK\r\n", 6U);
    TEST_CHECK(0 == service_process(now_us + 1U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_SERVER_START_WAIT == diagnostics.state);
    TEST_CHECK(!diagnostics.server_listening);

    mock_complete();
    TEST_CHECK(0 == service_process(now_us + 2U));
    TEST_CHECK(0 == service_reply(now_us + 3U, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_LISTENING));
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    return 0;
}

static int test_join_failure_backoff_reaches_reset (void)
{
    static uint64_t const backoff_us[] = {1000000U, 2000000U, 4000000U};
    esp_at_service_diagnostics_t diagnostics;
    uint64_t now_us = 2000002U;
    uint32_t attempt;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(0 == service_advance_initial_reset());

    for (attempt = 0U; attempt < 3U; attempt++)
    {
        char const * const reply = (0U == attempt) ?
            "\r\nWIFI CONNECTED\r\nWIFI GOT IP\r\nERROR\r\n" :
            "\r\nERROR\r\n";

        TEST_CHECK(0 == service_advance_to_join_wait(&now_us));
        TEST_CHECK(0 == service_reply(now_us, reply,
                                      ESP_AT_SERVICE_STATE_BACKOFF));
        esp_at_service_get_diagnostics(&diagnostics);
        TEST_CHECK((attempt + 1U) == diagnostics.recoveries);
        TEST_CHECK((attempt + 1U) == diagnostics.consecutive_failures);
        TEST_CHECK(!diagnostics.wifi_connected);
        TEST_CHECK(!diagnostics.got_ip);

        TEST_CHECK(0 == service_process(now_us + backoff_us[attempt] - 1U));
        esp_at_service_get_diagnostics(&diagnostics);
        TEST_CHECK(ESP_AT_SERVICE_STATE_BACKOFF == diagnostics.state);
        TEST_CHECK(0 == service_process(now_us + backoff_us[attempt]));
        esp_at_service_get_diagnostics(&diagnostics);
        if (attempt < 2U)
        {
            TEST_CHECK(ESP_AT_SERVICE_STATE_PROBE_SEND == diagnostics.state);
        }
        else
        {
            TEST_CHECK(ESP_AT_SERVICE_STATE_RESET_ASSERT == diagnostics.state);
        }
        now_us += backoff_us[attempt] + 10U;
    }

    TEST_CHECK(1U == diagnostics.reset_count);
    TEST_CHECK(0 == service_process(now_us));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_RESET_HOLD == diagnostics.state);
    TEST_CHECK(2U == diagnostics.reset_count);
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    return 0;
}

static int test_ip_query_retries_until_dhcp_ready (void)
{
    esp_at_service_diagnostics_t diagnostics;
    uint64_t now_us = 2000002U;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(0 == service_advance_initial_reset());
    TEST_CHECK(0 == service_advance_to_join_wait(&now_us));
    TEST_CHECK(0 == service_reply(now_us, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_IP_QUERY_SEND));
    now_us += 10U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_IP_QUERY_SEND,
                                       "AT+CIFSR\r\n"));
    now_us += 10U;
    TEST_CHECK(0 == service_reply(now_us,
                                  "\r\n+CIFSR:STAIP,\"0.0.0.0\"\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_IP_QUERY_RETRY_WAIT));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.ip_query_retries);
    TEST_CHECK(0U == diagnostics.recoveries);
    TEST_CHECK(!diagnostics.got_ip);

    TEST_CHECK(0 == service_process(now_us + 499999U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_IP_QUERY_RETRY_WAIT == diagnostics.state);
    TEST_CHECK(0 == service_process(now_us + 500000U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_IP_QUERY_SEND == diagnostics.state);
    now_us += 500001U;
    TEST_CHECK(0 == service_send_state(now_us,
                                       ESP_AT_SERVICE_STATE_IP_QUERY_SEND,
                                       "AT+CIFSR\r\n"));
    now_us += 10U;
    TEST_CHECK(0 == service_reply(now_us,
                                  "\r\nWIFI GOT IP\r\n"
                                  "+CIFSR:STAIP,\"192.168.10.26\"\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_MUX_SEND));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(diagnostics.got_ip);
    TEST_CHECK(0 == strcmp(diagnostics.station_ip, "192.168.10.26"));
    TEST_CHECK(0U == diagnostics.recoveries);
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    return 0;
}

static int test_ip_query_retry_limit_enters_recovery (void)
{
    esp_at_service_diagnostics_t diagnostics;
    uint64_t now_us = 2000002U;
    uint32_t attempt;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(0 == service_advance_initial_reset());
    TEST_CHECK(0 == service_advance_to_join_wait(&now_us));
    TEST_CHECK(0 == service_reply(now_us, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_IP_QUERY_SEND));

    for (attempt = 0U; attempt < 6U; attempt++)
    {
        now_us += 10U;
        TEST_CHECK(0 == service_send_state(now_us,
                                           ESP_AT_SERVICE_STATE_IP_QUERY_SEND,
                                           "AT+CIFSR\r\n"));
        now_us += 10U;
        TEST_CHECK(0 == service_reply(
            now_us,
            "\r\n+CIFSR:STAIP,\"0.0.0.0\"\r\nOK\r\n",
            (attempt < 5U) ? ESP_AT_SERVICE_STATE_IP_QUERY_RETRY_WAIT :
                             ESP_AT_SERVICE_STATE_BACKOFF));
        if (attempt < 5U)
        {
            TEST_CHECK(0 == service_process(now_us + 500000U));
            esp_at_service_get_diagnostics(&diagnostics);
            TEST_CHECK(ESP_AT_SERVICE_STATE_IP_QUERY_SEND == diagnostics.state);
            now_us += 500000U;
        }
    }

    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(5U == diagnostics.ip_query_retries);
    TEST_CHECK(1U == diagnostics.recoveries);
    TEST_CHECK(1U == diagnostics.command_errors);
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    return 0;
}

static int test_join_ignores_transient_disconnect_event (void)
{
    esp_at_service_diagnostics_t diagnostics;
    uint64_t now_us = 2000002U;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(0 == service_advance_initial_reset());
    TEST_CHECK(0 == service_advance_to_join_wait(&now_us));
    TEST_CHECK(0 == service_reply(now_us,
                                  "\r\nWIFI DISCONNECT\r\n"
                                  "WIFI CONNECTED\r\nWIFI GOT IP\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_IP_QUERY_SEND));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.wifi_disconnects);
    TEST_CHECK(0U == diagnostics.recoveries);
    TEST_CHECK(diagnostics.wifi_connected);
    TEST_CHECK(diagnostics.got_ip);
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    return 0;
}
#endif

static int test_probe_and_credentials_gate (void)
{
    esp_at_service_diagnostics_t diagnostics;
#if TEST_CREDENTIALS_CONFIGURED
    esp_at_service_state_t const expected_state = ESP_AT_SERVICE_STATE_WIFI_MODE_SEND;
#else
    esp_at_service_state_t const expected_state = ESP_AT_SERVICE_STATE_CREDENTIALS_WAIT;
#endif

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(0 == service_advance_initial_reset());
    TEST_CHECK(0 == service_send_state(2000002U,
                                       ESP_AT_SERVICE_STATE_PROBE_SEND,
                                       "AT\r\n"));
    TEST_CHECK(0 == service_reply(2000010U,
                                  "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_ECHO_SEND));
    TEST_CHECK(0 == service_send_state(2000011U,
                                       ESP_AT_SERVICE_STATE_ECHO_SEND,
                                       "ATE0\r\n"));
    TEST_CHECK(0 == service_reply(2000020U,
                                  "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_VERSION_SEND));
    TEST_CHECK(0 == service_send_state(2000021U,
                                       ESP_AT_SERVICE_STATE_VERSION_SEND,
                                       "AT+GMR\r\n"));
    TEST_CHECK(0 == service_reply(
        2000030U,
        "\r\nAT version:2.2.0.0-dev(esp8266)\r\nSDK version:v3.4\r\nOK\r\n",
        expected_state));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(0 == strcmp(diagnostics.firmware_version,
                           "AT version:2.2.0.0-dev(esp8266)"));
    TEST_CHECK((0 != TEST_CREDENTIALS_CONFIGURED) ==
               diagnostics.credentials_configured);
#if !TEST_CREDENTIALS_CONFIGURED
    TEST_CHECK(ESP_AT_NETWORK_CREDENTIALS_WAIT == diagnostics.network_state);
    TEST_CHECK(0 == service_process(30000000U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_CREDENTIALS_WAIT == diagnostics.state);
    TEST_CHECK(0U == diagnostics.recoveries);
#endif
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    return 0;
}

#if TEST_CREDENTIALS_CONFIGURED
static int test_join_server_and_tcp_session (void)
{
    uint8_t hello[128];
    uint8_t ipd[192];
    char ipd_header[32];
    uint32_t hello_bytes = 0U;
    int ipd_header_bytes;
    app_telemetry_snapshot_t snapshot = {0};
    esp_at_service_diagnostics_t diagnostics;
    uint32_t before_bytes;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_service_init(0U));
    TEST_CHECK(0 == service_advance_initial_reset());
    TEST_CHECK(0 == service_send_state(2000002U,
                                       ESP_AT_SERVICE_STATE_PROBE_SEND,
                                       "AT\r\n"));
    TEST_CHECK(0 == service_reply(2000011U, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_ECHO_SEND));
    TEST_CHECK(0 == service_send_state(2000012U,
                                       ESP_AT_SERVICE_STATE_ECHO_SEND,
                                       "ATE0\r\n"));
    TEST_CHECK(0 == service_reply(2000021U, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_VERSION_SEND));
    TEST_CHECK(0 == service_send_state(2000022U,
                                       ESP_AT_SERVICE_STATE_VERSION_SEND,
                                       "AT+GMR\r\n"));
    TEST_CHECK(0 == service_reply(2000031U, "\r\nAT version:test\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_WIFI_MODE_SEND));
    TEST_CHECK(0 == service_send_state(2000032U,
                                       ESP_AT_SERVICE_STATE_WIFI_MODE_SEND,
                                       "AT+CWMODE=1\r\n"));
    TEST_CHECK(0 == service_reply(2000041U, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_JOIN_SEND));
    TEST_CHECK(0 == service_send_state(2000042U,
                                       ESP_AT_SERVICE_STATE_JOIN_SEND,
                                       "AT+CWJAP=\"TestNet\",\"secret\"\r\n"));
    TEST_CHECK(0 == service_reply(2000051U,
                                  "\r\nWIFI CONNECTED\r\nWIFI GOT IP\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_IP_QUERY_SEND));
    TEST_CHECK(0 == service_send_state(2000052U,
                                       ESP_AT_SERVICE_STATE_IP_QUERY_SEND,
                                       "AT+CIFSR\r\n"));
    TEST_CHECK(0 == service_reply(2000061U,
                                  "\r\n+CIFSR:STAIP,\"192.168.10.26\"\r\n"
                                  "+CIFSR:STAMAC,\"aa:bb:cc:dd:ee:ff\"\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_MUX_SEND));
    TEST_CHECK(0 == service_send_state(2000062U,
                                       ESP_AT_SERVICE_STATE_MUX_SEND,
                                       "AT+CIPMUX=1\r\n"));
    TEST_CHECK(0 == service_reply(2000071U, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_SEND));
    TEST_CHECK(0 == service_send_state(2000072U,
                                       ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_SEND,
                                       "AT+CIPSERVERMAXCONN=5\r\n"));
    TEST_CHECK(0 == service_reply(2000081U, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_SERVER_STOP_SEND));
    TEST_CHECK(0 == service_send_state(2000082U,
                                       ESP_AT_SERVICE_STATE_SERVER_STOP_SEND,
                                       "AT+CIPSERVER=0\r\n"));
    TEST_CHECK(0 == service_reply(2000091U, "\r\nERROR\r\n",
                                  ESP_AT_SERVICE_STATE_SERVER_START_SEND));
    TEST_CHECK(0 == service_send_state(2000092U,
                                       ESP_AT_SERVICE_STATE_SERVER_START_SEND,
                                       "AT+CIPSERVER=1,80\r\n"));
    TEST_CHECK(0 == service_reply(2000101U, "\r\nOK\r\n",
                                  ESP_AT_SERVICE_STATE_LISTENING));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(0 == strcmp(diagnostics.station_ip, "192.168.10.26"));

    TEST_CHECK(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_HELLO,
        77U,
        RA8P_FLAG_ACK_REQUIRED,
        (uint8_t const *) "{}",
        2U,
        hello,
        sizeof(hello),
        &hello_bytes));
    inject_rx((uint8_t const *) "\r\n0,CONNECT\r\n",
              (uint32_t) strlen("\r\n0,CONNECT\r\n"));
    TEST_CHECK(0 == service_process(2000111U));
    ipd_header_bytes = snprintf(ipd_header, sizeof(ipd_header),
                                "+IPD,0,%lu:", (unsigned long) hello_bytes);
    TEST_CHECK((ipd_header_bytes > 0) &&
               ((size_t) ipd_header_bytes < sizeof(ipd_header)));
    (void) memcpy(ipd, ipd_header, (size_t) ipd_header_bytes);
    (void) memcpy(&ipd[ipd_header_bytes], hello, hello_bytes);
    inject_rx(ipd, (uint32_t) ipd_header_bytes + hello_bytes);
    TEST_CHECK(0 == service_process(2000121U));

    before_bytes = g_mock_transmitted_bytes;
    TEST_CHECK(0 == service_process(2000112U));
    mock_complete();
    TEST_CHECK(0 == service_process(2000113U));
    TEST_CHECK(capture_contains((uint8_t const *) "AT+CIPSEND=0,",
                                (uint32_t) strlen("AT+CIPSEND=0,")));
    inject_rx((uint8_t const *) "> ", (uint32_t) strlen("> "));
    TEST_CHECK(0 == service_process(2000114U));
    mock_complete();
    TEST_CHECK(0 == service_process(2000115U));
    TEST_CHECK(g_mock_transmitted_bytes > before_bytes);
    TEST_CHECK(capture_contains((uint8_t const *) "RA8P", 4U));
    inject_rx((uint8_t const *) "\r\nSEND OK\r\n",
              (uint32_t) strlen("\r\nSEND OK\r\n"));
    TEST_CHECK(0 == service_process(2000116U));

    snapshot.schema_version = APP_TELEMETRY_SCHEMA_VERSION;
    snapshot.sequence = 88U;
    snapshot.wheat_count = 12;
    (void) strcpy(snapshot.device_id, "ra8p1-test");
    (void) strcpy(snapshot.session_id, "lan-0");
    (void) strcpy(snapshot.build_version, "test");
    (void) strcpy(snapshot.model_version, "yolox-test");
    TEST_CHECK(APP_STATUS_OK == esp_at_service_queue_telemetry(&snapshot));
    TEST_CHECK(0 == service_process(2000121U));
    mock_complete();
    TEST_CHECK(0 == service_process(2000122U));
    inject_rx((uint8_t const *) ">", (uint32_t) strlen(">"));
    TEST_CHECK(0 == service_process(2000123U));
    mock_complete();
    TEST_CHECK(0 == service_process(2000124U));
    inject_rx((uint8_t const *) "\r\nSEND OK\r\n",
              (uint32_t) strlen("\r\nSEND OK\r\n"));
    TEST_CHECK(0 == service_process(2000125U));

    snapshot.sequence = 89U;
    TEST_CHECK(APP_STATUS_OK == esp_at_service_queue_telemetry(&snapshot));
    TEST_CHECK(0 == service_process(2000126U));
    mock_complete();
    TEST_CHECK(0 == service_process(2000127U));
    inject_rx((uint8_t const *) "\r\nERROR\r\n",
              (uint32_t) strlen("\r\nERROR\r\n"));
    TEST_CHECK(0 == service_process(2000128U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(LAN_SESSION_ROUTER_NO_LINK == diagnostics.active_link_id);
    TEST_CHECK(1U == diagnostics.send_failures);
    TEST_CHECK(0 == service_process(2000129U));
    mock_complete();
    TEST_CHECK(0 == service_process(2000130U));
    TEST_CHECK(capture_contains((uint8_t const *) "AT+CIPCLOSE=0\r\n",
                                (uint32_t) strlen("AT+CIPCLOSE=0\r\n")));
    inject_rx((uint8_t const *) "\r\nOK\r\n",
              (uint32_t) strlen("\r\nOK\r\n"));
    TEST_CHECK(0 == service_process(2000131U));

    inject_rx((uint8_t const *) "\r\nWIFI DISCONNECT\r\n",
              (uint32_t) strlen("\r\nWIFI DISCONNECT\r\n"));
    TEST_CHECK(0 == service_process(2000141U));
    esp_at_service_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_SERVICE_STATE_BACKOFF == diagnostics.state);
    TEST_CHECK(ESP_AT_NETWORK_BACKOFF == diagnostics.network_state);
    TEST_CHECK(1U == diagnostics.wifi_disconnects);
    TEST_CHECK(2U == diagnostics.send_completions);
    TEST_CHECK(1U == diagnostics.send_failures);
    TEST_CHECK(0U == diagnostics.parser_errors);
    TEST_CHECK(0U < diagnostics.ipd_bytes);
    TEST_CHECK(APP_STATUS_OK == esp_at_service_close());
    return 0;
}
#endif

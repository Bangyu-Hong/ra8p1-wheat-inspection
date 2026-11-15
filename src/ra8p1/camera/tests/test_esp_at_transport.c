#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_at_transport.h"

#define TEST_CAPTURE_BYTES   (ESP_AT_TRANSPORT_TX_BYTES * 3U)

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
static uint32_t g_mock_open_calls;
static uint32_t g_mock_close_calls;
static uint32_t g_mock_write_calls;
static uint32_t g_mock_abort_calls;
static fsp_err_t g_mock_open_status;
static fsp_err_t g_mock_close_status;
static fsp_err_t g_mock_write_status;
static fsp_err_t g_mock_abort_status;

static fsp_err_t mock_open(uart_ctrl_t * const p_ctrl, uart_cfg_t const * const p_cfg);
static fsp_err_t mock_write(uart_ctrl_t * const p_ctrl,
                            uint8_t const * const p_src,
                            uint32_t const bytes);
static fsp_err_t mock_close(uart_ctrl_t * const p_ctrl);
static fsp_err_t mock_abort(uart_ctrl_t * const p_ctrl, uart_dir_t direction);
static void mock_reset(void);
static void mock_complete(void);
static void inject_rx(uint32_t count, uint8_t seed);
static void fill_sequence(uint8_t * p_data, uint32_t count, uint8_t seed);
static int test_rx_ring_overflow_and_recovery(void);
static int test_tx_request_boundaries_and_wrap(void);
static int test_timeout_drops_only_active_request(void);
static int test_abort_failure_and_late_completion(void);
static int test_queue_and_lifecycle_bounds(void);
static int test_write_start_error_retains_ownership(void);

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

int main (void)
{
    TEST_CHECK(0 == test_rx_ring_overflow_and_recovery());
    TEST_CHECK(0 == test_tx_request_boundaries_and_wrap());
    TEST_CHECK(0 == test_timeout_drops_only_active_request());
    TEST_CHECK(0 == test_abort_failure_and_late_completion());
    TEST_CHECK(0 == test_queue_and_lifecycle_bounds());
    TEST_CHECK(0 == test_write_start_error_retains_ownership());

    (void) printf("esp-at-transport status=ok rx=8191-owned tx_requests=16 "
                  "wrap=owned timeout=request-scoped\n");
    return 0;
}

static fsp_err_t mock_open (uart_ctrl_t * const p_ctrl, uart_cfg_t const * const p_cfg)
{
    if ((NULL == p_ctrl) || (NULL == p_cfg))
    {
        return FSP_ERR_INTERNAL;
    }
    g_mock_open_calls++;
    return g_mock_open_status;
}

static fsp_err_t mock_write (uart_ctrl_t * const p_ctrl,
                             uint8_t const * const p_src,
                             uint32_t const bytes)
{
    fsp_err_t const status = g_mock_write_status;

    if ((NULL == p_ctrl) || (NULL == p_src) || (0U == bytes) ||
        (NULL != g_mock_active_source))
    {
        return FSP_ERR_IN_USE;
    }
    g_mock_write_calls++;
    g_mock_write_status = FSP_SUCCESS;
    if (FSP_SUCCESS == status)
    {
        g_mock_active_source = p_src;
        g_mock_active_bytes = bytes;
    }
    return status;
}

static fsp_err_t mock_close (uart_ctrl_t * const p_ctrl)
{
    if (NULL == p_ctrl)
    {
        return FSP_ERR_INTERNAL;
    }
    g_mock_close_calls++;
    return g_mock_close_status;
}

static fsp_err_t mock_abort (uart_ctrl_t * const p_ctrl, uart_dir_t direction)
{
    fsp_err_t const status = g_mock_abort_status;

    if ((NULL == p_ctrl) || (UART_DIR_TX != direction))
    {
        return FSP_ERR_INTERNAL;
    }
    g_mock_abort_calls++;
    g_mock_abort_status = FSP_SUCCESS;
    if (FSP_SUCCESS == status)
    {
        g_mock_active_source = NULL;
        g_mock_active_bytes = 0U;
    }
    return status;
}

static void mock_reset (void)
{
    (void) esp_at_transport_close();
    g_mock_active_source = NULL;
    g_mock_active_bytes = 0U;
    g_mock_transmitted_bytes = 0U;
    g_mock_open_calls = 0U;
    g_mock_close_calls = 0U;
    g_mock_write_calls = 0U;
    g_mock_abort_calls = 0U;
    g_mock_open_status = FSP_SUCCESS;
    g_mock_close_status = FSP_SUCCESS;
    g_mock_write_status = FSP_SUCCESS;
    g_mock_abort_status = FSP_SUCCESS;
    (void) memset(g_mock_transmitted, 0, sizeof(g_mock_transmitted));
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

static void inject_rx (uint32_t count, uint8_t seed)
{
    uart_callback_args_t args = {.channel = 0U, .event = UART_EVENT_RX_CHAR};
    uint32_t index;

    for (index = 0U; index < count; index++)
    {
        args.data = (uint32_t) (uint8_t) (seed + (uint8_t) index);
        esp_at_uart0_callback(&args);
    }
}

static void fill_sequence (uint8_t * p_data, uint32_t count, uint8_t seed)
{
    uint32_t index;

    for (index = 0U; index < count; index++)
    {
        p_data[index] = (uint8_t) (seed + (uint8_t) index);
    }
}

static int test_rx_ring_overflow_and_recovery (void)
{
    uint8_t output[ESP_AT_TRANSPORT_RX_CAPACITY];
    uint32_t output_bytes;
    esp_at_transport_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_init());
    inject_rx(ESP_AT_TRANSPORT_RX_CAPACITY + 3U, 0x20U);
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_read(
        output, (uint32_t) sizeof(output), &output_bytes));
    TEST_CHECK(ESP_AT_TRANSPORT_RX_CAPACITY == output_bytes);
    TEST_CHECK(0x20U == output[0U]);
    TEST_CHECK((uint8_t) (0x20U + (uint8_t) (ESP_AT_TRANSPORT_RX_CAPACITY - 1U)) ==
               output[ESP_AT_TRANSPORT_RX_CAPACITY - 1U]);
    esp_at_transport_get_diagnostics(&diagnostics);
    TEST_CHECK((ESP_AT_TRANSPORT_RX_CAPACITY + 3U) == diagnostics.rx_bytes);
    TEST_CHECK(3U == diagnostics.rx_overflows);
    TEST_CHECK(ESP_AT_TRANSPORT_RX_CAPACITY == diagnostics.rx_high_water);
    TEST_CHECK(0U == diagnostics.rx_buffered_bytes);

    inject_rx(2U, 0xA0U);
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_read(output, 1U, &output_bytes));
    TEST_CHECK(1U == output_bytes);
    TEST_CHECK(0xA0U == output[0U]);
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_read(output, 1U, &output_bytes));
    TEST_CHECK(0xA1U == output[0U]);
    return 0;
}

static int test_tx_request_boundaries_and_wrap (void)
{
    uint8_t first[7000U];
    uint8_t second[2000U];
    uint8_t expected[sizeof(first) + sizeof(second)];
    esp_at_transport_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_init());
    fill_sequence(first, (uint32_t) sizeof(first), 0x11U);
    fill_sequence(second, (uint32_t) sizeof(second), 0x81U);
    (void) memcpy(expected, first, sizeof(first));
    (void) memcpy(&expected[sizeof(first)], second, sizeof(second));

    TEST_CHECK(APP_STATUS_OK == esp_at_transport_write(first, sizeof(first)));
    TEST_CHECK(sizeof(first) == g_mock_active_bytes);
    mock_complete();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_process(1000U));
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_write(second, sizeof(second)));
    TEST_CHECK(1192U == g_mock_active_bytes);
    mock_complete();
    TEST_CHECK(APP_STATUS_BUSY == esp_at_transport_process(2000U));
    TEST_CHECK(808U == g_mock_active_bytes);
    mock_complete();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_process(3000U));
    TEST_CHECK(sizeof(expected) == g_mock_transmitted_bytes);
    TEST_CHECK(0 == memcmp(expected, g_mock_transmitted, sizeof(expected)));

    esp_at_transport_get_diagnostics(&diagnostics);
    TEST_CHECK(2U == diagnostics.tx_requests);
    TEST_CHECK(2U == diagnostics.tx_completed_requests);
    TEST_CHECK(3U == diagnostics.tx_completions);
    TEST_CHECK(sizeof(expected) == diagnostics.tx_bytes);
    TEST_CHECK(0U == diagnostics.tx_buffered_requests);
    return 0;
}

static int test_timeout_drops_only_active_request (void)
{
    uint8_t first[2000U];
    static uint8_t const second[] = "AT\r\n";
    esp_at_transport_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_init());
    fill_sequence(first, (uint32_t) sizeof(first), 0x30U);
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_write(first, sizeof(first)));
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_write(second, sizeof(second) - 1U));
    TEST_CHECK(APP_STATUS_BUSY == esp_at_transport_process(1000U));
    TEST_CHECK(APP_STATUS_BUSY == esp_at_transport_process(274611U));
    TEST_CHECK(APP_STATUS_TIMEOUT == esp_at_transport_process(274612U));
    TEST_CHECK(1U == g_mock_abort_calls);

    esp_at_transport_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.tx_timeouts);
    TEST_CHECK(1U == diagnostics.tx_dropped_requests);
    TEST_CHECK(sizeof(first) == diagnostics.tx_dropped_bytes);
    TEST_CHECK(1U == diagnostics.tx_buffered_requests);
    TEST_CHECK((sizeof(second) - 1U) == diagnostics.tx_buffered_bytes);

    TEST_CHECK(APP_STATUS_BUSY == esp_at_transport_process(275000U));
    TEST_CHECK((sizeof(second) - 1U) == g_mock_active_bytes);
    mock_complete();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_process(276000U));
    TEST_CHECK((sizeof(second) - 1U) == g_mock_transmitted_bytes);
    TEST_CHECK(0 == memcmp(second, g_mock_transmitted, sizeof(second) - 1U));
    return 0;
}

static int test_abort_failure_and_late_completion (void)
{
    uint8_t data[1U] = {0x5AU};
    esp_at_transport_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_init());
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_write(data, sizeof(data)));
    TEST_CHECK(APP_STATUS_BUSY == esp_at_transport_process(1000U));
    g_mock_abort_status = FSP_ERR_INTERNAL;
    TEST_CHECK(APP_STATUS_TIMEOUT == esp_at_transport_process(101087U));
    TEST_CHECK(APP_STATUS_TIMEOUT == esp_at_transport_process(500000U));
    TEST_CHECK(1U == g_mock_abort_calls);
    mock_complete();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_process(500001U));

    esp_at_transport_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.tx_timeouts);
    TEST_CHECK(1U == diagnostics.tx_abort_errors);
    TEST_CHECK(1U == diagnostics.tx_completed_requests);
    TEST_CHECK(1U == diagnostics.tx_bytes);
    return 0;
}

static int test_queue_and_lifecycle_bounds (void)
{
    uint8_t byte = 0xA5U;
    uint32_t index;
    esp_at_transport_diagnostics_t diagnostics;

    mock_reset();
    g_mock_open_status = FSP_ERR_INTERNAL;
    TEST_CHECK(APP_STATUS_IO == esp_at_transport_init());
    esp_at_transport_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.open_errors);
    TEST_CHECK(!diagnostics.initialized);

    g_mock_open_status = FSP_SUCCESS;
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_init());
    for (index = 0U; index < ESP_AT_TRANSPORT_TX_REQUESTS; index++)
    {
        TEST_CHECK(APP_STATUS_OK == esp_at_transport_write(&byte, 1U));
    }
    TEST_CHECK(APP_STATUS_BUSY == esp_at_transport_write(&byte, 1U));
    esp_at_transport_get_diagnostics(&diagnostics);
    TEST_CHECK(ESP_AT_TRANSPORT_TX_REQUESTS == diagnostics.tx_request_high_water);
    TEST_CHECK(1U == diagnostics.tx_dropped_requests);
    TEST_CHECK(1U == diagnostics.tx_dropped_bytes);

    TEST_CHECK(APP_STATUS_OK == esp_at_transport_close());
    TEST_CHECK(1U == g_mock_abort_calls);
    TEST_CHECK(1U == g_mock_close_calls);
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_close());
    TEST_CHECK(1U == g_mock_close_calls);
    TEST_CHECK(APP_STATUS_NOT_INITIALIZED == esp_at_transport_write(&byte, 1U));
    return 0;
}

static int test_write_start_error_retains_ownership (void)
{
    static uint8_t const data[] = "ATE0\r\n";
    esp_at_transport_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_init());
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_init());
    TEST_CHECK(1U == g_mock_open_calls);
    g_mock_write_status = FSP_ERR_INTERNAL;
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_write(data, sizeof(data) - 1U));
    esp_at_transport_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.tx_errors);
    TEST_CHECK(FSP_ERR_INTERNAL == diagnostics.last_fsp_error);
    TEST_CHECK(1U == diagnostics.tx_buffered_requests);
    TEST_CHECK((sizeof(data) - 1U) == diagnostics.tx_buffered_bytes);

    TEST_CHECK(APP_STATUS_BUSY == esp_at_transport_process(1000U));
    mock_complete();
    TEST_CHECK(APP_STATUS_OK == esp_at_transport_process(2000U));
    TEST_CHECK((sizeof(data) - 1U) == g_mock_transmitted_bytes);
    TEST_CHECK(0 == memcmp(data, g_mock_transmitted, sizeof(data) - 1U));
    return 0;
}

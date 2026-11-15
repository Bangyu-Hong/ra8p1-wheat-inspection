#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "uart_console.h"

#define TEST_UART_QUEUE_BYTES      (4096U)
#define TEST_CAPTURE_BYTES         (TEST_UART_QUEUE_BYTES * 3U)

#define TEST_CHECK(condition)                                                       \
    do                                                                              \
    {                                                                               \
        if (!(condition))                                                           \
        {                                                                           \
            (void) printf("check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static uint8_t const * g_mock_active_source;
static uint32_t        g_mock_active_length;
static uint8_t         g_mock_transmitted[TEST_CAPTURE_BYTES];
static uint32_t        g_mock_transmitted_length;
static uint32_t        g_mock_open_calls;
static uint32_t        g_mock_write_calls;
static uint32_t        g_mock_abort_calls;
static fsp_err_t       g_mock_next_write_status;
static fsp_err_t       g_mock_next_abort_status;

static fsp_err_t mock_open(uart_ctrl_t * const p_ctrl, uart_cfg_t const * const p_cfg);
static fsp_err_t mock_write(uart_ctrl_t * const p_ctrl, uint8_t const * const p_src, uint32_t const bytes);
static fsp_err_t mock_abort(uart_ctrl_t * const p_ctrl, uart_dir_t communication_to_abort);
static void      mock_reset(void);
static void      mock_complete(void);
static void      fill_sequence(uint8_t * p_data, uint32_t length, uint8_t seed);
static int       test_wraparound_and_in_flight_ownership(void);
static int       test_full_queue_drop_and_recovery(void);
static int       test_callback_and_api_error_accounting(void);
static int       test_timeout_abort_and_recovery(void);
static int       test_abort_failure_is_not_retried_in_a_tight_loop(void);

static uart_api_t const g_mock_uart_api =
{
    .open  = mock_open,
    .write = mock_write,
    .communicationAbort = mock_abort,
};

static uint32_t g_mock_uart_ctrl;
static uart_cfg_t const g_mock_uart_cfg = {0};

const uart_instance_t g_uart9 =
{
    .p_ctrl = (uart_ctrl_t *) &g_mock_uart_ctrl,
    .p_cfg  = &g_mock_uart_cfg,
    .p_api  = &g_mock_uart_api,
};

static fsp_err_t mock_open (uart_ctrl_t * const p_ctrl, uart_cfg_t const * const p_cfg)
{
    if ((NULL == p_ctrl) || (NULL == p_cfg))
    {
        return FSP_ERR_INTERNAL;
    }

    g_mock_open_calls++;
    return FSP_SUCCESS;
}

static fsp_err_t mock_write (uart_ctrl_t * const p_ctrl,
                             uint8_t const * const p_src,
                             uint32_t const bytes)
{
    fsp_err_t const status = g_mock_next_write_status;

    if ((NULL == p_ctrl) || (NULL == p_src) || (0U == bytes) ||
        (NULL != g_mock_active_source))
    {
        return FSP_ERR_IN_USE;
    }

    g_mock_write_calls++;
    g_mock_next_write_status = FSP_SUCCESS;
    if (FSP_SUCCESS != status)
    {
        return status;
    }

    g_mock_active_source = p_src;
    g_mock_active_length = bytes;
    return FSP_SUCCESS;
}

static fsp_err_t mock_abort (uart_ctrl_t * const p_ctrl, uart_dir_t communication_to_abort)
{
    fsp_err_t const status = g_mock_next_abort_status;

    if ((NULL == p_ctrl) || (UART_DIR_TX != communication_to_abort))
    {
        return FSP_ERR_INTERNAL;
    }

    g_mock_abort_calls++;
    g_mock_next_abort_status = FSP_SUCCESS;
    if (FSP_SUCCESS == status)
    {
        g_mock_active_source = NULL;
        g_mock_active_length = 0U;
    }

    return status;
}

static void mock_reset (void)
{
    g_mock_active_source     = NULL;
    g_mock_active_length     = 0U;
    g_mock_transmitted_length = 0U;
    g_mock_open_calls        = 0U;
    g_mock_write_calls       = 0U;
    g_mock_abort_calls       = 0U;
    g_mock_next_write_status = FSP_SUCCESS;
    g_mock_next_abort_status = FSP_SUCCESS;
    (void) memset(g_mock_transmitted, 0, sizeof(g_mock_transmitted));
}

static void mock_complete (void)
{
    uart_callback_args_t callback_args =
    {
        .channel = 9U,
        .event   = UART_EVENT_TX_COMPLETE,
    };

    if ((NULL == g_mock_active_source) ||
        (g_mock_active_length > (TEST_CAPTURE_BYTES - g_mock_transmitted_length)))
    {
        return;
    }

    (void) memcpy(&g_mock_transmitted[g_mock_transmitted_length],
                  g_mock_active_source,
                  g_mock_active_length);
    g_mock_transmitted_length += g_mock_active_length;
    g_mock_active_source = NULL;
    g_mock_active_length = 0U;
    camera_uart9_callback(&callback_args);
}

static void fill_sequence (uint8_t * p_data, uint32_t length, uint8_t seed)
{
    for (uint32_t index = 0U; index < length; index++)
    {
        p_data[index] = (uint8_t) (seed + (uint8_t) index);
    }
}

static int test_wraparound_and_in_flight_ownership (void)
{
    uint8_t first[3000U];
    uint8_t second[1000U];
    uint8_t wrapped[2000U];
    uint8_t expected[6000U];
    uart_console_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_init());
    TEST_CHECK(1U == g_mock_open_calls);
    fill_sequence(first, (uint32_t) sizeof(first), 0x10U);
    fill_sequence(second, (uint32_t) sizeof(second), 0x40U);
    fill_sequence(wrapped, (uint32_t) sizeof(wrapped), 0x80U);
    (void) memcpy(expected, first, sizeof(first));
    (void) memcpy(&expected[sizeof(first)], second, sizeof(second));
    (void) memcpy(&expected[sizeof(first) + sizeof(second)], wrapped, sizeof(wrapped));

    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_write(first, sizeof(first)));
    TEST_CHECK(3000U == g_mock_active_length);
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_write(second, sizeof(second)));
    TEST_CHECK(3000U == g_mock_active_length);

    mock_complete();
    TEST_CHECK(UART_CONSOLE_STATUS_BUSY == uart_console_process(1000U));
    TEST_CHECK(1000U == g_mock_active_length);
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_write(wrapped, sizeof(wrapped)));

    mock_complete();
    TEST_CHECK(UART_CONSOLE_STATUS_BUSY == uart_console_process(2000U));
    TEST_CHECK(96U == g_mock_active_length);
    mock_complete();
    TEST_CHECK(UART_CONSOLE_STATUS_BUSY == uart_console_process(3000U));
    TEST_CHECK(1904U == g_mock_active_length);
    mock_complete();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_process(4000U));

    TEST_CHECK(sizeof(expected) == g_mock_transmitted_length);
    TEST_CHECK(0 == memcmp(expected, g_mock_transmitted, sizeof(expected)));
    uart_console_get_diagnostics(&diagnostics);
    TEST_CHECK(3U == diagnostics.tx_requests);
    TEST_CHECK(4U == diagnostics.tx_completions);
    TEST_CHECK(sizeof(expected) == diagnostics.tx_bytes);
    TEST_CHECK(0U == diagnostics.queued_bytes);
    TEST_CHECK(4000U == diagnostics.queue_high_water);
    TEST_CHECK(!diagnostics.tx_in_progress);

    return 0;
}

static int test_full_queue_drop_and_recovery (void)
{
    uint8_t full[TEST_UART_QUEUE_BYTES];
    uint8_t extra[1U] = {0xAAU};
    uart_console_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_init());
    fill_sequence(full, (uint32_t) sizeof(full), 0x20U);

    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_write(full, sizeof(full)));
    TEST_CHECK(TEST_UART_QUEUE_BYTES == g_mock_active_length);
    TEST_CHECK(UART_CONSOLE_STATUS_BUSY == uart_console_write(extra, sizeof(extra)));
    uart_console_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.dropped_requests);
    TEST_CHECK(1U == diagnostics.dropped_bytes);
    TEST_CHECK(TEST_UART_QUEUE_BYTES == diagnostics.queued_bytes);
    TEST_CHECK(TEST_UART_QUEUE_BYTES == diagnostics.queue_high_water);

    mock_complete();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_process(1000U));
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_write(extra, sizeof(extra)));
    mock_complete();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_process(2000U));
    TEST_CHECK((TEST_UART_QUEUE_BYTES + 1U) == g_mock_transmitted_length);
    TEST_CHECK(0 == memcmp(full, g_mock_transmitted, sizeof(full)));
    TEST_CHECK(extra[0] == g_mock_transmitted[TEST_UART_QUEUE_BYTES]);

    return 0;
}

static int test_callback_and_api_error_accounting (void)
{
    uint8_t data[8U] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    uart_callback_args_t unexpected =
    {
        .channel = 9U,
        .event   = UART_EVENT_RX_CHAR,
    };
    uart_console_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_init());
    g_mock_next_write_status = FSP_ERR_INTERNAL;
    TEST_CHECK(UART_CONSOLE_STATUS_API_ERROR == uart_console_write(data, sizeof(data)));
    TEST_CHECK(NULL == g_mock_active_source);

    uart_console_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.api_errors);
    TEST_CHECK(FSP_ERR_INTERNAL == diagnostics.last_fsp_error);
    TEST_CHECK(sizeof(data) == diagnostics.queued_bytes);

    TEST_CHECK(UART_CONSOLE_STATUS_BUSY == uart_console_process(1000U));
    TEST_CHECK(sizeof(data) == g_mock_active_length);
    camera_uart9_callback(NULL);
    camera_uart9_callback(&unexpected);
    TEST_CHECK(UART_CONSOLE_STATUS_BUSY == uart_console_process(2000U));
    TEST_CHECK(sizeof(data) == g_mock_active_length);
    mock_complete();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_process(3000U));

    uart_console_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.tx_requests);
    TEST_CHECK(1U == diagnostics.tx_completions);
    TEST_CHECK(2U == diagnostics.unexpected_events);
    TEST_CHECK(sizeof(data) == diagnostics.tx_bytes);
    TEST_CHECK(0U == diagnostics.queued_bytes);

    return 0;
}

static int test_timeout_abort_and_recovery (void)
{
    uint8_t data[100U];
    uint8_t retry[2U] = {0x5AU, 0xA5U};
    uart_console_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_init());
    fill_sequence(data, (uint32_t) sizeof(data), 0x30U);

    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_write(data, sizeof(data)));
    TEST_CHECK(UART_CONSOLE_STATUS_BUSY == uart_console_process(1000U));
    TEST_CHECK(UART_CONSOLE_STATUS_BUSY == uart_console_process(109680U));
    TEST_CHECK(UART_CONSOLE_STATUS_TIMEOUT == uart_console_process(109681U));
    TEST_CHECK(1U == g_mock_abort_calls);
    TEST_CHECK(NULL == g_mock_active_source);

    uart_console_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.timeouts);
    TEST_CHECK(1U == diagnostics.dropped_requests);
    TEST_CHECK(sizeof(data) == diagnostics.dropped_bytes);
    TEST_CHECK(0U == diagnostics.queued_bytes);
    TEST_CHECK(0U == diagnostics.tx_bytes);
    TEST_CHECK(!diagnostics.tx_in_progress);

    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_write(retry, sizeof(retry)));
    mock_complete();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_process(110000U));
    TEST_CHECK(sizeof(retry) == g_mock_transmitted_length);
    TEST_CHECK(0 == memcmp(retry, g_mock_transmitted, sizeof(retry)));

    return 0;
}

static int test_abort_failure_is_not_retried_in_a_tight_loop (void)
{
    uint8_t data[1U] = {0x33U};
    uart_console_diagnostics_t diagnostics;

    mock_reset();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_init());
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_write(data, sizeof(data)));
    TEST_CHECK(UART_CONSOLE_STATUS_BUSY == uart_console_process(1000U));
    g_mock_next_abort_status = FSP_ERR_INTERNAL;
    TEST_CHECK(UART_CONSOLE_STATUS_TIMEOUT == uart_console_process(101087U));
    TEST_CHECK(1U == g_mock_abort_calls);
    TEST_CHECK(UART_CONSOLE_STATUS_TIMEOUT == uart_console_process(1000000U));
    TEST_CHECK(1U == g_mock_abort_calls);

    uart_console_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.timeouts);
    TEST_CHECK(1U == diagnostics.abort_errors);
    TEST_CHECK(FSP_ERR_INTERNAL == diagnostics.last_abort_error);
    TEST_CHECK(diagnostics.tx_in_progress);
    TEST_CHECK(sizeof(data) == diagnostics.queued_bytes);

    mock_complete();
    TEST_CHECK(UART_CONSOLE_STATUS_OK == uart_console_process(1000001U));
    TEST_CHECK(sizeof(data) == g_mock_transmitted_length);

    return 0;
}

int main (void)
{
    TEST_CHECK(0 == test_wraparound_and_in_flight_ownership());
    TEST_CHECK(0 == test_full_queue_drop_and_recovery());
    TEST_CHECK(0 == test_callback_and_api_error_accounting());
    TEST_CHECK(0 == test_timeout_abort_and_recovery());
    TEST_CHECK(0 == test_abort_failure_is_not_retried_in_a_tight_loop());

    (void) printf("uart-console-queue status=ok bytes=%u wrap=ok in_flight=owned drop=bounded timeout=wire-aware\n",
                  TEST_UART_QUEUE_BYTES);
    return 0;
}

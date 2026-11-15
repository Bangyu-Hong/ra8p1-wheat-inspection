#include "uart_console.h"

#include <string.h>

#if !defined(UART_CONSOLE_HOST_TEST)
 #include <errno.h>
 #include <limits.h>
 #include <sys/unistd.h>
#endif

#include "hal_data.h"

#define UART_CONSOLE_TX_BUFFER_SIZE   (4096U)
#define UART_CONSOLE_BAUD_RATE        (115200ULL)
#define UART_CONSOLE_BITS_PER_BYTE    (10ULL)
#define UART_CONSOLE_TX_MARGIN_US     (100000ULL)
#define UART_CONSOLE_US_PER_SECOND    (1000000ULL)

static volatile bool         g_tx_complete;
static volatile bool         g_tx_in_progress;
static volatile uart_event_t g_last_event;
static volatile uint32_t     g_tx_completions;
static volatile uint32_t     g_unexpected_events;

static uint8_t  g_tx_buffer[UART_CONSOLE_TX_BUFFER_SIZE];
static uint32_t g_tx_head;
static uint32_t g_tx_tail;
static uint32_t g_tx_used;
static uint32_t g_tx_active_length;
static bool     g_tx_deadline_valid;
static bool     g_tx_abort_failed;
static uint64_t g_tx_deadline_us;

static uart_console_diagnostics_t g_diagnostics;

#if !defined(UART_CONSOLE_HOST_TEST)
_READ_WRITE_RETURN_TYPE _write(int file, void const * p_buffer, size_t length);
#endif

static uart_console_status_t uart_console_record_api_error(fsp_err_t error);
static void                  uart_console_copy_to_queue(uint8_t const * p_data, uint32_t length);
static uart_console_status_t uart_console_process_internal(bool time_valid, uint64_t now_us);
static uint64_t              uart_console_timeout_us(uint32_t length);
static void                  uart_console_arm_deadline(uint64_t now_us);
static uart_console_status_t uart_console_abort_timed_out_transfer(void);

uart_console_status_t uart_console_init (void)
{
    fsp_err_t err;

    g_tx_complete       = false;
    g_tx_in_progress    = false;
    g_last_event        = (uart_event_t) 0;
    g_tx_completions    = 0U;
    g_unexpected_events = 0U;
    g_tx_head           = 0U;
    g_tx_tail           = 0U;
    g_tx_used           = 0U;
    g_tx_active_length  = 0U;
    g_tx_deadline_valid = false;
    g_tx_abort_failed   = false;
    g_tx_deadline_us    = 0U;
    g_diagnostics       = (uart_console_diagnostics_t) {0};

    err = g_uart9.p_api->open(g_uart9.p_ctrl, g_uart9.p_cfg);
    if (FSP_SUCCESS != err)
    {
        return uart_console_record_api_error(err);
    }

    g_diagnostics.initialized    = true;
    g_diagnostics.last_fsp_error = FSP_SUCCESS;
    g_diagnostics.last_status    = UART_CONSOLE_STATUS_OK;

    return UART_CONSOLE_STATUS_OK;
}

uart_console_status_t uart_console_write (uint8_t const * p_data, size_t length)
{
    uart_console_status_t status;

    if ((!g_diagnostics.initialized) || (NULL == p_data))
    {
        g_diagnostics.last_status = g_diagnostics.initialized ?
                                    UART_CONSOLE_STATUS_INVALID_ARGUMENT :
                                    UART_CONSOLE_STATUS_NOT_INITIALIZED;
        return g_diagnostics.last_status;
    }

    if (0U == length)
    {
        g_diagnostics.last_status = UART_CONSOLE_STATUS_OK;
        return UART_CONSOLE_STATUS_OK;
    }

    if (length > UINT32_MAX)
    {
        g_diagnostics.last_status = UART_CONSOLE_STATUS_INVALID_ARGUMENT;
        return UART_CONSOLE_STATUS_INVALID_ARGUMENT;
    }

    if (length > (size_t) (UART_CONSOLE_TX_BUFFER_SIZE - g_tx_used))
    {
        g_diagnostics.dropped_requests++;
        g_diagnostics.dropped_bytes += (uint32_t) length;
        g_diagnostics.last_status = UART_CONSOLE_STATUS_BUSY;
        return UART_CONSOLE_STATUS_BUSY;
    }

    g_diagnostics.tx_requests++;
    uart_console_copy_to_queue(p_data, (uint32_t) length);
    g_diagnostics.queued_bytes = g_tx_used;
    if (g_tx_used > g_diagnostics.queue_high_water)
    {
        g_diagnostics.queue_high_water = g_tx_used;
    }

    status = uart_console_process_internal(false, 0U);
    return (UART_CONSOLE_STATUS_BUSY == status) ? UART_CONSOLE_STATUS_OK : status;
}

uart_console_status_t uart_console_process (uint64_t now_us)
{
    return uart_console_process_internal(true, now_us);
}

static uart_console_status_t uart_console_process_internal (bool time_valid, uint64_t now_us)
{
    uint32_t contiguous_length;
    fsp_err_t err;

    if (!g_diagnostics.initialized)
    {
        g_diagnostics.last_status = UART_CONSOLE_STATUS_NOT_INITIALIZED;
        return UART_CONSOLE_STATUS_NOT_INITIALIZED;
    }

    if (g_tx_in_progress)
    {
        if (!g_tx_complete)
        {
            if (g_tx_abort_failed)
            {
                g_diagnostics.last_status = UART_CONSOLE_STATUS_TIMEOUT;
                return UART_CONSOLE_STATUS_TIMEOUT;
            }

            if (time_valid)
            {
                if (!g_tx_deadline_valid)
                {
                    uart_console_arm_deadline(now_us);
                }
                else if (now_us >= g_tx_deadline_us)
                {
                    return uart_console_abort_timed_out_transfer();
                }
            }

            g_diagnostics.last_status = UART_CONSOLE_STATUS_BUSY;
            return UART_CONSOLE_STATUS_BUSY;
        }

        g_tx_tail = (g_tx_tail + g_tx_active_length) % UART_CONSOLE_TX_BUFFER_SIZE;
        g_tx_used -= g_tx_active_length;
        g_diagnostics.tx_bytes += g_tx_active_length;
        g_diagnostics.queued_bytes = g_tx_used;
        g_tx_active_length = 0U;
        g_tx_complete = false;
        g_tx_in_progress = false;
        g_tx_deadline_valid = false;
        g_tx_abort_failed = false;
        g_tx_deadline_us = 0U;
    }

    if (0U == g_tx_used)
    {
        g_diagnostics.last_status = UART_CONSOLE_STATUS_OK;
        return UART_CONSOLE_STATUS_OK;
    }

    contiguous_length = UART_CONSOLE_TX_BUFFER_SIZE - g_tx_tail;
    if (contiguous_length > g_tx_used)
    {
        contiguous_length = g_tx_used;
    }

    g_tx_complete = false;
    g_tx_in_progress = true;
    g_tx_active_length = contiguous_length;
    g_tx_deadline_valid = false;
    g_tx_abort_failed = false;
    g_tx_deadline_us = 0U;
    err = g_uart9.p_api->write(g_uart9.p_ctrl, &g_tx_buffer[g_tx_tail], contiguous_length);
    if (FSP_SUCCESS != err)
    {
        g_tx_active_length = 0U;
        g_tx_in_progress = false;
        return uart_console_record_api_error(err);
    }
    if (time_valid)
    {
        uart_console_arm_deadline(now_us);
    }

    g_diagnostics.last_status = UART_CONSOLE_STATUS_BUSY;
    return UART_CONSOLE_STATUS_BUSY;
}

static void uart_console_copy_to_queue (uint8_t const * p_data, uint32_t length)
{
    uint32_t const first_length = ((UART_CONSOLE_TX_BUFFER_SIZE - g_tx_head) < length) ?
                                  (UART_CONSOLE_TX_BUFFER_SIZE - g_tx_head) : length;

    (void) memcpy(&g_tx_buffer[g_tx_head], p_data, first_length);
    if (first_length < length)
    {
        (void) memcpy(g_tx_buffer, &p_data[first_length], length - first_length);
    }
    g_tx_head = (g_tx_head + length) % UART_CONSOLE_TX_BUFFER_SIZE;
    g_tx_used += length;
}

static uint64_t uart_console_timeout_us (uint32_t length)
{
    uint64_t const bit_us_numerator = (uint64_t) length * UART_CONSOLE_BITS_PER_BYTE *
                                      UART_CONSOLE_US_PER_SECOND;
    uint64_t const wire_time_us = (bit_us_numerator + UART_CONSOLE_BAUD_RATE - 1ULL) /
                                  UART_CONSOLE_BAUD_RATE;

    return wire_time_us + UART_CONSOLE_TX_MARGIN_US;
}

static void uart_console_arm_deadline (uint64_t now_us)
{
    uint64_t const timeout_us = uart_console_timeout_us(g_tx_active_length);

    g_tx_deadline_us = (now_us > (UINT64_MAX - timeout_us)) ?
                       UINT64_MAX :
                       now_us + timeout_us;
    g_tx_deadline_valid = true;
}

static uart_console_status_t uart_console_abort_timed_out_transfer (void)
{
    fsp_err_t const err = g_uart9.p_api->communicationAbort(g_uart9.p_ctrl, UART_DIR_TX);

    g_diagnostics.timeouts++;
    g_diagnostics.last_abort_error = err;
    g_diagnostics.last_status = UART_CONSOLE_STATUS_TIMEOUT;
    if (FSP_SUCCESS != err)
    {
        g_diagnostics.abort_errors++;
        g_tx_deadline_valid = false;
        g_tx_abort_failed = true;
        return UART_CONSOLE_STATUS_TIMEOUT;
    }

    g_tx_tail = (g_tx_tail + g_tx_active_length) % UART_CONSOLE_TX_BUFFER_SIZE;
    g_tx_used -= g_tx_active_length;
    g_diagnostics.dropped_requests++;
    g_diagnostics.dropped_bytes += g_tx_active_length;
    g_diagnostics.queued_bytes = g_tx_used;
    g_tx_active_length = 0U;
    g_tx_complete = false;
    g_tx_in_progress = false;
    g_tx_deadline_valid = false;
    g_tx_abort_failed = false;
    g_tx_deadline_us = 0U;

    return UART_CONSOLE_STATUS_TIMEOUT;
}

/* The queue owns bytes until TX_COMPLETE, so printf can return immediately. */
static uart_console_status_t uart_console_record_api_error (fsp_err_t error)
{
    g_diagnostics.api_errors++;
    g_diagnostics.last_fsp_error = error;
    g_diagnostics.last_status    = UART_CONSOLE_STATUS_API_ERROR;

    return UART_CONSOLE_STATUS_API_ERROR;
}

void uart_console_get_diagnostics (uart_console_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        g_diagnostics.tx_completions    = g_tx_completions;
        g_diagnostics.unexpected_events = g_unexpected_events;
        g_diagnostics.last_event        = g_last_event;
        g_diagnostics.tx_in_progress    = g_tx_in_progress;
        g_diagnostics.queued_bytes      = g_tx_used;
        *p_diagnostics                  = g_diagnostics;
    }
}

/* Keep the ISR bounded: acknowledge completion and let app_poll advance the queue. */
void camera_uart9_callback (uart_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        g_unexpected_events++;
        return;
    }

    g_last_event = p_args->event;
    if ((UART_EVENT_TX_COMPLETE == p_args->event) && g_tx_in_progress)
    {
        g_tx_completions++;
        g_tx_complete = true;
    }
    else
    {
        g_unexpected_events++;
    }
}

/* stdio reports success after bounded enqueue, not after wire transmission. */
#if !defined(UART_CONSOLE_HOST_TEST)
_READ_WRITE_RETURN_TYPE _write (int file, void const * p_buffer, size_t length)
{
    uart_console_status_t status;

    if ((STDOUT_FILENO != file) && (STDERR_FILENO != file))
    {
        errno = EBADF;
        return -1;
    }

    if ((NULL == p_buffer) || (length > (size_t) INT_MAX))
    {
        errno = EINVAL;
        return -1;
    }

    status = uart_console_write((uint8_t const *) p_buffer, length);
    if (UART_CONSOLE_STATUS_BUSY == status)
    {
        return (_READ_WRITE_RETURN_TYPE) length;
    }
    if (UART_CONSOLE_STATUS_OK != status)
    {
        errno = EIO;
        return -1;
    }

    return (_READ_WRITE_RETURN_TYPE) length;
}
#endif

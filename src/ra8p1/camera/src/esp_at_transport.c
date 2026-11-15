#include "esp_at_transport.h"

#include <limits.h>
#include <string.h>

#include "hal_data.h"

#define ESP_AT_TRANSPORT_BAUD_RATE       (115200ULL)
#define ESP_AT_TRANSPORT_BITS_PER_BYTE   (10ULL)
#define ESP_AT_TRANSPORT_US_PER_SECOND   (1000000ULL)
#define ESP_AT_TRANSPORT_TX_MARGIN_US    (100000ULL)

typedef struct st_esp_at_tx_request
{
    uint32_t remaining_bytes;
} esp_at_tx_request_t;

static uint8_t g_rx_buffer[ESP_AT_TRANSPORT_RX_BYTES];
static volatile uint32_t g_rx_head;
static volatile uint32_t g_rx_tail;
static volatile uint32_t g_rx_bytes;
static volatile uint32_t g_rx_high_water;
static volatile uint32_t g_rx_overflows;
static volatile uint32_t g_rx_errors;
static volatile uint32_t g_tx_completions;
static volatile uint32_t g_unexpected_events;
static volatile uart_event_t g_last_event;

static uint8_t g_tx_buffer[ESP_AT_TRANSPORT_TX_BYTES];
static esp_at_tx_request_t g_tx_requests[ESP_AT_TRANSPORT_TX_REQUESTS];
static uint32_t g_tx_head;
static uint32_t g_tx_tail;
static uint32_t g_tx_used;
static uint32_t g_tx_request_head;
static uint32_t g_tx_request_tail;
static uint32_t g_tx_request_used;
static uint32_t g_tx_active_bytes;
static volatile bool g_tx_complete;
static volatile bool g_tx_in_progress;
static bool g_tx_deadline_valid;
static bool g_tx_abort_failed;
static uint64_t g_tx_deadline_us;
static esp_at_transport_diagnostics_t g_diagnostics;

static uint32_t esp_at_transport_rx_used(void);
static void esp_at_transport_copy_to_tx(uint8_t const * p_data, uint32_t data_bytes);
static app_status_t esp_at_transport_process_internal(bool time_valid, uint64_t now_us);
static void esp_at_transport_finish_tx_chunk(void);
static void esp_at_transport_drop_active_request(void);
static void esp_at_transport_reset_tx_active(void);
static uint64_t esp_at_transport_timeout_us(uint32_t data_bytes);
static void esp_at_transport_snapshot_isr_diagnostics(void);
static app_status_t esp_at_transport_record(app_status_t status);

app_status_t esp_at_transport_init (void)
{
    fsp_err_t err;

    if (g_diagnostics.initialized)
    {
        return esp_at_transport_record(APP_STATUS_OK);
    }

    g_rx_head = 0U;
    g_rx_tail = 0U;
    g_rx_bytes = 0U;
    g_rx_high_water = 0U;
    g_rx_overflows = 0U;
    g_rx_errors = 0U;
    g_tx_completions = 0U;
    g_unexpected_events = 0U;
    g_last_event = (uart_event_t) 0;
    g_tx_head = 0U;
    g_tx_tail = 0U;
    g_tx_used = 0U;
    g_tx_request_head = 0U;
    g_tx_request_tail = 0U;
    g_tx_request_used = 0U;
    g_tx_active_bytes = 0U;
    g_tx_complete = false;
    g_tx_in_progress = false;
    g_tx_deadline_valid = false;
    g_tx_abort_failed = false;
    g_tx_deadline_us = 0U;
    g_diagnostics = (esp_at_transport_diagnostics_t) {0};

    err = g_uart0.p_api->open(g_uart0.p_ctrl, g_uart0.p_cfg);
    if (FSP_SUCCESS != err)
    {
        g_diagnostics.open_errors++;
        g_diagnostics.last_fsp_error = err;
        return esp_at_transport_record(APP_STATUS_IO);
    }

    g_diagnostics.initialized = true;
    g_diagnostics.last_fsp_error = FSP_SUCCESS;
    return esp_at_transport_record(APP_STATUS_OK);
}

app_status_t esp_at_transport_close (void)
{
    fsp_err_t err;

    if (!g_diagnostics.initialized)
    {
        return esp_at_transport_record(APP_STATUS_OK);
    }
    if (g_tx_in_progress)
    {
        if (g_tx_complete)
        {
            esp_at_transport_finish_tx_chunk();
        }
        else
        {
            err = g_uart0.p_api->communicationAbort(g_uart0.p_ctrl, UART_DIR_TX);
            g_diagnostics.last_fsp_error = err;
            if (FSP_SUCCESS != err)
            {
                g_diagnostics.tx_abort_errors++;
                return esp_at_transport_record(APP_STATUS_IO);
            }
            esp_at_transport_drop_active_request();
        }
    }

    err = g_uart0.p_api->close(g_uart0.p_ctrl);
    g_diagnostics.last_fsp_error = err;
    if (FSP_SUCCESS != err)
    {
        g_diagnostics.close_errors++;
        return esp_at_transport_record(APP_STATUS_IO);
    }

    g_diagnostics.tx_dropped_requests += g_tx_request_used;
    g_diagnostics.tx_dropped_bytes += g_tx_used;
    g_tx_used = 0U;
    g_tx_request_used = 0U;
    esp_at_transport_reset_tx_active();
    g_diagnostics.tx_buffered_bytes = 0U;
    g_diagnostics.tx_buffered_requests = 0U;
    g_diagnostics.initialized = false;
    return esp_at_transport_record(APP_STATUS_OK);
}

app_status_t esp_at_transport_write (uint8_t const * p_data, size_t data_bytes)
{
    uint32_t bytes;

    if (!g_diagnostics.initialized)
    {
        return esp_at_transport_record(APP_STATUS_NOT_INITIALIZED);
    }
    if (NULL == p_data)
    {
        return esp_at_transport_record(APP_STATUS_INVALID_ARGUMENT);
    }
    if (0U == data_bytes)
    {
        return esp_at_transport_record(APP_STATUS_OK);
    }
    if (data_bytes > UINT32_MAX)
    {
        return esp_at_transport_record(APP_STATUS_INVALID_ARGUMENT);
    }
    bytes = (uint32_t) data_bytes;
    if ((bytes > (ESP_AT_TRANSPORT_TX_BYTES - g_tx_used)) ||
        (g_tx_request_used >= ESP_AT_TRANSPORT_TX_REQUESTS))
    {
        g_diagnostics.tx_dropped_requests++;
        g_diagnostics.tx_dropped_bytes += bytes;
        return esp_at_transport_record(APP_STATUS_BUSY);
    }

    esp_at_transport_copy_to_tx(p_data, bytes);
    g_tx_requests[g_tx_request_head].remaining_bytes = bytes;
    g_tx_request_head = (g_tx_request_head + 1U) % ESP_AT_TRANSPORT_TX_REQUESTS;
    g_tx_request_used++;
    g_diagnostics.tx_requests++;
    g_diagnostics.tx_buffered_bytes = g_tx_used;
    g_diagnostics.tx_buffered_requests = g_tx_request_used;
    if (g_tx_used > g_diagnostics.tx_high_water)
    {
        g_diagnostics.tx_high_water = g_tx_used;
    }
    if (g_tx_request_used > g_diagnostics.tx_request_high_water)
    {
        g_diagnostics.tx_request_high_water = g_tx_request_used;
    }

    (void) esp_at_transport_process_internal(false, 0U);
    return esp_at_transport_record(APP_STATUS_OK);
}

app_status_t esp_at_transport_read (uint8_t * p_output,
                                    uint32_t output_capacity,
                                    uint32_t * p_output_bytes)
{
    uint32_t copied = 0U;

    if (!g_diagnostics.initialized)
    {
        return esp_at_transport_record(APP_STATUS_NOT_INITIALIZED);
    }
    if ((NULL == p_output) || (0U == output_capacity) || (NULL == p_output_bytes))
    {
        return esp_at_transport_record(APP_STATUS_INVALID_ARGUMENT);
    }

    while ((copied < output_capacity) && (g_rx_tail != g_rx_head))
    {
        p_output[copied++] = g_rx_buffer[g_rx_tail];
        g_rx_tail = (g_rx_tail + 1U) % ESP_AT_TRANSPORT_RX_BYTES;
    }
    *p_output_bytes = copied;
    g_diagnostics.rx_buffered_bytes = esp_at_transport_rx_used();
    return esp_at_transport_record(APP_STATUS_OK);
}

void esp_at_transport_discard_rx (void)
{
    g_rx_tail = g_rx_head;
    g_diagnostics.rx_buffered_bytes = 0U;
}

app_status_t esp_at_transport_process (uint64_t now_us)
{
    return esp_at_transport_process_internal(true, now_us);
}

static app_status_t esp_at_transport_process_internal (bool time_valid, uint64_t now_us)
{
    uint32_t contiguous_bytes;
    fsp_err_t err;

    if (!g_diagnostics.initialized)
    {
        return esp_at_transport_record(APP_STATUS_NOT_INITIALIZED);
    }

    if (g_tx_in_progress)
    {
        if (g_tx_complete)
        {
            esp_at_transport_finish_tx_chunk();
        }
        else if (g_tx_abort_failed)
        {
            return esp_at_transport_record(APP_STATUS_TIMEOUT);
        }
        else
        {
            if (time_valid)
            {
                if (!g_tx_deadline_valid)
                {
                    uint64_t const timeout_us = esp_at_transport_timeout_us(g_tx_active_bytes);

                    g_tx_deadline_us = (now_us > (UINT64_MAX - timeout_us)) ?
                                       UINT64_MAX : now_us + timeout_us;
                    g_tx_deadline_valid = true;
                }
                else if (now_us >= g_tx_deadline_us)
                {
                    err = g_uart0.p_api->communicationAbort(g_uart0.p_ctrl, UART_DIR_TX);
                    g_diagnostics.tx_timeouts++;
                    g_diagnostics.last_fsp_error = err;
                    g_tx_deadline_valid = false;
                    if (FSP_SUCCESS != err)
                    {
                        g_diagnostics.tx_abort_errors++;
                        g_tx_abort_failed = true;
                        return esp_at_transport_record(APP_STATUS_TIMEOUT);
                    }
                    esp_at_transport_drop_active_request();
                    return esp_at_transport_record(APP_STATUS_TIMEOUT);
                }
            }
            return esp_at_transport_record(APP_STATUS_BUSY);
        }
    }

    if (0U == g_tx_request_used)
    {
        return esp_at_transport_record(APP_STATUS_OK);
    }

    contiguous_bytes = ESP_AT_TRANSPORT_TX_BYTES - g_tx_tail;
    if (contiguous_bytes > g_tx_requests[g_tx_request_tail].remaining_bytes)
    {
        contiguous_bytes = g_tx_requests[g_tx_request_tail].remaining_bytes;
    }
    g_tx_active_bytes = contiguous_bytes;
    g_tx_complete = false;
    g_tx_in_progress = true;
    g_tx_abort_failed = false;
    g_tx_deadline_valid = false;
    g_tx_deadline_us = 0U;
    err = g_uart0.p_api->write(g_uart0.p_ctrl,
                               &g_tx_buffer[g_tx_tail],
                               contiguous_bytes);
    if (FSP_SUCCESS != err)
    {
        g_tx_active_bytes = 0U;
        g_tx_in_progress = false;
        g_diagnostics.tx_errors++;
        g_diagnostics.last_fsp_error = err;
        return esp_at_transport_record(APP_STATUS_IO);
    }
    if (time_valid)
    {
        uint64_t const timeout_us = esp_at_transport_timeout_us(contiguous_bytes);

        g_tx_deadline_us = (now_us > (UINT64_MAX - timeout_us)) ?
                           UINT64_MAX : now_us + timeout_us;
        g_tx_deadline_valid = true;
    }
    return esp_at_transport_record(APP_STATUS_BUSY);
}

void esp_at_transport_get_diagnostics (esp_at_transport_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        esp_at_transport_snapshot_isr_diagnostics();
        g_diagnostics.rx_buffered_bytes = esp_at_transport_rx_used();
        g_diagnostics.tx_buffered_bytes = g_tx_used;
        g_diagnostics.tx_buffered_requests = g_tx_request_used;
        g_diagnostics.tx_in_progress = g_tx_in_progress;
        *p_diagnostics = g_diagnostics;
    }
}

void esp_at_uart0_callback (uart_callback_args_t * p_args)
{
    uint32_t next_head;

    if (NULL == p_args)
    {
        g_unexpected_events++;
        return;
    }

    g_last_event = p_args->event;
    if (UART_EVENT_RX_CHAR == p_args->event)
    {
        uint32_t used;

        g_rx_bytes++;
        next_head = (g_rx_head + 1U) % ESP_AT_TRANSPORT_RX_BYTES;
        if (next_head == g_rx_tail)
        {
            g_rx_overflows++;
            return;
        }
        g_rx_buffer[g_rx_head] = (uint8_t) p_args->data;
        g_rx_head = next_head;
        used = esp_at_transport_rx_used();
        if (used > g_rx_high_water)
        {
            g_rx_high_water = used;
        }
    }
    else if ((UART_EVENT_TX_COMPLETE == p_args->event) && g_tx_in_progress)
    {
        g_tx_completions++;
        g_tx_complete = true;
    }
    else if ((UART_EVENT_ERR_PARITY == p_args->event) ||
             (UART_EVENT_ERR_FRAMING == p_args->event) ||
             (UART_EVENT_ERR_OVERFLOW == p_args->event) ||
             (UART_EVENT_BREAK_DETECT == p_args->event))
    {
        g_rx_errors++;
    }
    else
    {
        g_unexpected_events++;
    }
}

static uint32_t esp_at_transport_rx_used (void)
{
    uint32_t const head = g_rx_head;
    uint32_t const tail = g_rx_tail;

    return (head >= tail) ? (head - tail) :
           (ESP_AT_TRANSPORT_RX_BYTES - tail + head);
}

static void esp_at_transport_copy_to_tx (uint8_t const * p_data, uint32_t data_bytes)
{
    uint32_t first_bytes = ESP_AT_TRANSPORT_TX_BYTES - g_tx_head;

    if (first_bytes > data_bytes)
    {
        first_bytes = data_bytes;
    }
    (void) memcpy(&g_tx_buffer[g_tx_head], p_data, first_bytes);
    if (first_bytes < data_bytes)
    {
        (void) memcpy(g_tx_buffer, &p_data[first_bytes], data_bytes - first_bytes);
    }
    g_tx_head = (g_tx_head + data_bytes) % ESP_AT_TRANSPORT_TX_BYTES;
    g_tx_used += data_bytes;
}

static void esp_at_transport_finish_tx_chunk (void)
{
    esp_at_tx_request_t * const p_request = &g_tx_requests[g_tx_request_tail];

    g_tx_tail = (g_tx_tail + g_tx_active_bytes) % ESP_AT_TRANSPORT_TX_BYTES;
    g_tx_used -= g_tx_active_bytes;
    p_request->remaining_bytes -= g_tx_active_bytes;
    g_diagnostics.tx_bytes += g_tx_active_bytes;
    if (0U == p_request->remaining_bytes)
    {
        g_tx_request_tail = (g_tx_request_tail + 1U) % ESP_AT_TRANSPORT_TX_REQUESTS;
        g_tx_request_used--;
        g_diagnostics.tx_completed_requests++;
    }
    g_diagnostics.tx_buffered_bytes = g_tx_used;
    g_diagnostics.tx_buffered_requests = g_tx_request_used;
    esp_at_transport_reset_tx_active();
}

static void esp_at_transport_drop_active_request (void)
{
    uint32_t const dropped_bytes = g_tx_requests[g_tx_request_tail].remaining_bytes;

    g_tx_tail = (g_tx_tail + dropped_bytes) % ESP_AT_TRANSPORT_TX_BYTES;
    g_tx_used -= dropped_bytes;
    g_tx_request_tail = (g_tx_request_tail + 1U) % ESP_AT_TRANSPORT_TX_REQUESTS;
    g_tx_request_used--;
    g_diagnostics.tx_dropped_requests++;
    g_diagnostics.tx_dropped_bytes += dropped_bytes;
    g_diagnostics.tx_buffered_bytes = g_tx_used;
    g_diagnostics.tx_buffered_requests = g_tx_request_used;
    esp_at_transport_reset_tx_active();
}

static void esp_at_transport_reset_tx_active (void)
{
    g_tx_active_bytes = 0U;
    g_tx_complete = false;
    g_tx_in_progress = false;
    g_tx_deadline_valid = false;
    g_tx_abort_failed = false;
    g_tx_deadline_us = 0U;
}

static uint64_t esp_at_transport_timeout_us (uint32_t data_bytes)
{
    uint64_t const numerator = (uint64_t) data_bytes *
                               ESP_AT_TRANSPORT_BITS_PER_BYTE *
                               ESP_AT_TRANSPORT_US_PER_SECOND;
    uint64_t const wire_us = (numerator + ESP_AT_TRANSPORT_BAUD_RATE - 1ULL) /
                             ESP_AT_TRANSPORT_BAUD_RATE;

    return wire_us + ESP_AT_TRANSPORT_TX_MARGIN_US;
}

static void esp_at_transport_snapshot_isr_diagnostics (void)
{
    g_diagnostics.rx_bytes = g_rx_bytes;
    g_diagnostics.rx_high_water = g_rx_high_water;
    g_diagnostics.rx_overflows = g_rx_overflows;
    g_diagnostics.rx_errors = g_rx_errors;
    g_diagnostics.tx_completions = g_tx_completions;
    g_diagnostics.unexpected_events = g_unexpected_events;
    g_diagnostics.last_event = g_last_event;
}

static app_status_t esp_at_transport_record (app_status_t status)
{
    g_diagnostics.last_status = status;
    return status;
}

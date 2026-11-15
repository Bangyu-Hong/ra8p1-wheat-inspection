#ifndef ESP_AT_TRANSPORT_H_
#define ESP_AT_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_status.h"
#include "r_uart_api.h"

#define ESP_AT_TRANSPORT_RX_BYTES  (8192U)
#define ESP_AT_TRANSPORT_TX_BYTES  (8192U)
#define ESP_AT_TRANSPORT_RX_CAPACITY  (ESP_AT_TRANSPORT_RX_BYTES - 1U)
#define ESP_AT_TRANSPORT_TX_REQUESTS  (16U)

typedef struct st_esp_at_transport_diagnostics
{
    uint32_t rx_bytes;
    uint32_t rx_buffered_bytes;
    uint32_t rx_high_water;
    uint32_t rx_overflows;
    uint32_t rx_errors;
    uint32_t tx_requests;
    uint32_t tx_completions;
    uint32_t tx_completed_requests;
    uint32_t tx_bytes;
    uint32_t tx_buffered_bytes;
    uint32_t tx_buffered_requests;
    uint32_t tx_high_water;
    uint32_t tx_request_high_water;
    uint32_t tx_dropped_requests;
    uint32_t tx_dropped_bytes;
    uint32_t tx_errors;
    uint32_t tx_timeouts;
    uint32_t tx_abort_errors;
    uint32_t open_errors;
    uint32_t close_errors;
    uint32_t unexpected_events;
    fsp_err_t last_fsp_error;
    uart_event_t last_event;
    app_status_t last_status;
    bool initialized;
    bool tx_in_progress;
} esp_at_transport_diagnostics_t;

app_status_t esp_at_transport_init(void);
app_status_t esp_at_transport_close(void);
app_status_t esp_at_transport_write(uint8_t const * p_data, size_t data_bytes);
app_status_t esp_at_transport_read(uint8_t * p_output,
                                   uint32_t output_capacity,
                                   uint32_t * p_output_bytes);
void esp_at_transport_discard_rx(void);
app_status_t esp_at_transport_process(uint64_t now_us);
void esp_at_transport_get_diagnostics(esp_at_transport_diagnostics_t * p_diagnostics);
void esp_at_uart0_callback(uart_callback_args_t * p_args);

#endif /* ESP_AT_TRANSPORT_H_ */

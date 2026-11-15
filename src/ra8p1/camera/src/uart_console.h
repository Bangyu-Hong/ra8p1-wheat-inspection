#ifndef UART_CONSOLE_H_
#define UART_CONSOLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "r_uart_api.h"

typedef enum e_uart_console_status
{
    UART_CONSOLE_STATUS_OK = 0,
    UART_CONSOLE_STATUS_NOT_INITIALIZED,
    UART_CONSOLE_STATUS_INVALID_ARGUMENT,
    UART_CONSOLE_STATUS_BUSY,
    UART_CONSOLE_STATUS_API_ERROR,
    UART_CONSOLE_STATUS_TIMEOUT
} uart_console_status_t;

typedef struct st_uart_console_diagnostics
{
    uint32_t              tx_requests;
    uint32_t              tx_completions;
    uint32_t              tx_bytes;
    uint32_t              queued_bytes;
    uint32_t              queue_high_water;
    uint32_t              dropped_requests;
    uint32_t              dropped_bytes;
    uint32_t              api_errors;
    uint32_t              timeouts;
    uint32_t              abort_errors;
    uint32_t              unexpected_events;
    fsp_err_t             last_fsp_error;
    fsp_err_t             last_abort_error;
    uart_event_t          last_event;
    uart_console_status_t last_status;
    bool                  initialized;
    bool                  tx_in_progress;
} uart_console_diagnostics_t;

uart_console_status_t uart_console_init(void);
uart_console_status_t uart_console_write(uint8_t const * p_data, size_t length);
uart_console_status_t uart_console_process(uint64_t now_us);
void                  uart_console_get_diagnostics(uart_console_diagnostics_t * p_diagnostics);
void                  camera_uart9_callback(uart_callback_args_t * p_args);

#endif /* UART_CONSOLE_H_ */

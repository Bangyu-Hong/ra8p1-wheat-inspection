#ifndef TEST_STUB_R_UART_API_H_
#define TEST_STUB_R_UART_API_H_

#include <stdint.h>

typedef int32_t fsp_err_t;

#define FSP_SUCCESS          ((fsp_err_t) 0)
#define FSP_ERR_IN_USE       ((fsp_err_t) 1)
#define FSP_ERR_INTERNAL     ((fsp_err_t) 2)

typedef enum e_uart_event
{
    UART_EVENT_RX_COMPLETE   = (1UL << 0),
    UART_EVENT_TX_COMPLETE   = (1UL << 1),
    UART_EVENT_RX_CHAR       = (1UL << 2),
    UART_EVENT_ERR_PARITY    = (1UL << 3),
    UART_EVENT_ERR_FRAMING   = (1UL << 4),
    UART_EVENT_ERR_OVERFLOW  = (1UL << 5),
    UART_EVENT_BREAK_DETECT  = (1UL << 6),
    UART_EVENT_TX_DATA_EMPTY = (1UL << 7),
} uart_event_t;

typedef enum e_uart_dir
{
    UART_DIR_RX_TX = 3U,
    UART_DIR_RX    = 1U,
    UART_DIR_TX    = 2U,
} uart_dir_t;

typedef struct st_uart_callback_arg
{
    uint32_t     channel;
    uart_event_t event;
    uint32_t     data;
    void       * p_context;
} uart_callback_args_t;

typedef void uart_ctrl_t;

typedef struct st_uart_cfg
{
    uint32_t unused;
} uart_cfg_t;

typedef struct st_uart_api
{
    fsp_err_t (* open)(uart_ctrl_t * const p_ctrl, uart_cfg_t const * const p_cfg);
    fsp_err_t (* write)(uart_ctrl_t * const p_ctrl, uint8_t const * const p_src, uint32_t const bytes);
    fsp_err_t (* close)(uart_ctrl_t * const p_ctrl);
    fsp_err_t (* communicationAbort)(uart_ctrl_t * const p_ctrl, uart_dir_t communication_to_abort);
} uart_api_t;

typedef struct st_uart_instance
{
    uart_ctrl_t      * p_ctrl;
    uart_cfg_t const * p_cfg;
    uart_api_t const * p_api;
} uart_instance_t;

#endif /* TEST_STUB_R_UART_API_H_ */

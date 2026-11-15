#ifndef SCCB_BUS_H_
#define SCCB_BUS_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "r_i2c_master_api.h"

#define SCCB_BUS_INSTANCE_NAME       "IIC0"
#define SCCB_BUS_RATE_HZ             (100000U)
#define SCCB_BUS_SLAVE_ADDRESS       (0x3CU)
#define SCCB_BUS_MAX_TIMEOUT_US      (10000U)
#define SCCB_BUS_MAX_ATTEMPTS        (3U)

typedef struct st_sccb_bus_diagnostics
{
    uint32_t           transfer_requests;
    uint32_t           transfer_completions;
    uint32_t           register_reads;
    uint32_t           register_writes;
    uint32_t           retries;
    uint32_t           api_errors;
    uint32_t           timeouts;
    uint32_t           aborted_events;
    uint32_t           abort_requests;
    uint32_t           abort_errors;
    uint32_t           unexpected_events;
    uint16_t           last_register;
    fsp_err_t          last_fsp_error;
    fsp_err_t          last_abort_error;
    i2c_master_event_t last_event;
    app_status_t       last_status;
    bool               initialized;
    bool               transfer_in_progress;
} sccb_bus_diagnostics_t;

app_status_t sccb_bus_init(void);
app_status_t sccb_read_u8(uint16_t reg, uint8_t * p_value, uint32_t timeout_us);
app_status_t sccb_write_u8(uint16_t reg, uint8_t value, uint32_t timeout_us);
void         sccb_bus_get_diagnostics(sccb_bus_diagnostics_t * p_diagnostics);
void         camera_i2c0_callback(i2c_master_callback_args_t * p_args);

#endif /* SCCB_BUS_H_ */

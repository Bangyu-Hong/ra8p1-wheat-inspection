#include "sccb_bus.h"

#include <stddef.h>

#include "hal_data.h"

#define SCCB_BUS_WAIT_STEP_US       (10U)

typedef enum e_sccb_transfer_direction
{
    SCCB_TRANSFER_WRITE = 0,
    SCCB_TRANSFER_READ
} sccb_transfer_direction_t;

static volatile bool               g_transfer_complete;
static volatile bool               g_transfer_aborted;
static volatile bool               g_transfer_in_progress;
static volatile i2c_master_event_t g_expected_event;
static volatile i2c_master_event_t g_last_event;
static volatile uint32_t           g_transfer_completions;
static volatile uint32_t           g_aborted_events;
static volatile uint32_t           g_unexpected_events;

static sccb_bus_diagnostics_t g_diagnostics;

static app_status_t sccb_transfer(sccb_transfer_direction_t direction,
                                  uint8_t * p_data,
                                  uint32_t length,
                                  bool restart,
                                  uint32_t timeout_us,
                                  bool * p_retryable);
static void         sccb_abort(void);
static app_status_t sccb_record_status(app_status_t status);

app_status_t sccb_bus_init (void)
{
    fsp_err_t err;

    g_transfer_complete    = false;
    g_transfer_aborted     = false;
    g_transfer_in_progress = false;
    g_expected_event       = (i2c_master_event_t) 0;
    g_last_event           = (i2c_master_event_t) 0;
    g_transfer_completions = 0U;
    g_aborted_events       = 0U;
    g_unexpected_events    = 0U;
    g_diagnostics          = (sccb_bus_diagnostics_t) {0};

    err = g_i2c_master0.p_api->open(g_i2c_master0.p_ctrl, g_i2c_master0.p_cfg);
    if (FSP_SUCCESS != err)
    {
        g_diagnostics.api_errors++;
        g_diagnostics.last_fsp_error = err;
        return sccb_record_status(APP_STATUS_IO);
    }

    g_diagnostics.initialized     = true;
    g_diagnostics.last_fsp_error  = FSP_SUCCESS;
    g_diagnostics.last_abort_error = FSP_SUCCESS;

    return sccb_record_status(APP_STATUS_OK);
}

app_status_t sccb_read_u8 (uint16_t reg, uint8_t * p_value, uint32_t timeout_us)
{
    app_status_t status = APP_STATUS_IO;
    uint8_t      address[2];
    uint8_t      value = 0U;

    if (!g_diagnostics.initialized)
    {
        return sccb_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    if ((NULL == p_value) || (0U == timeout_us) || (timeout_us > SCCB_BUS_MAX_TIMEOUT_US))
    {
        return sccb_record_status(APP_STATUS_INVALID_ARGUMENT);
    }

    g_diagnostics.last_register = reg;
    address[0] = (uint8_t) (reg >> 8U);
    address[1] = (uint8_t) reg;

    for (uint32_t attempt = 0U; attempt < SCCB_BUS_MAX_ATTEMPTS; attempt++)
    {
        bool retryable = false;

        status = sccb_transfer(SCCB_TRANSFER_WRITE,
                               address,
                               (uint32_t) sizeof(address),
                               true,
                               timeout_us,
                               &retryable);
        if (APP_STATUS_OK == status)
        {
            status = sccb_transfer(SCCB_TRANSFER_READ,
                                   &value,
                                   1U,
                                   false,
                                   timeout_us,
                                   &retryable);
        }

        if (APP_STATUS_OK == status)
        {
            *p_value = value;
            g_diagnostics.register_reads++;
            return sccb_record_status(APP_STATUS_OK);
        }

        if ((!retryable) || ((attempt + 1U) >= SCCB_BUS_MAX_ATTEMPTS))
        {
            break;
        }

        g_diagnostics.retries++;
    }

    return sccb_record_status(status);
}

app_status_t sccb_write_u8 (uint16_t reg, uint8_t value, uint32_t timeout_us)
{
    app_status_t status = APP_STATUS_IO;
    uint8_t      payload[3];

    if (!g_diagnostics.initialized)
    {
        return sccb_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    if ((0U == timeout_us) || (timeout_us > SCCB_BUS_MAX_TIMEOUT_US))
    {
        return sccb_record_status(APP_STATUS_INVALID_ARGUMENT);
    }

    g_diagnostics.last_register = reg;
    payload[0] = (uint8_t) (reg >> 8U);
    payload[1] = (uint8_t) reg;
    payload[2] = value;

    for (uint32_t attempt = 0U; attempt < SCCB_BUS_MAX_ATTEMPTS; attempt++)
    {
        bool retryable = false;

        status = sccb_transfer(SCCB_TRANSFER_WRITE,
                               payload,
                               (uint32_t) sizeof(payload),
                               false,
                               timeout_us,
                               &retryable);
        if (APP_STATUS_OK == status)
        {
            g_diagnostics.register_writes++;
            return sccb_record_status(APP_STATUS_OK);
        }

        if ((!retryable) || ((attempt + 1U) >= SCCB_BUS_MAX_ATTEMPTS))
        {
            break;
        }

        g_diagnostics.retries++;
    }

    return sccb_record_status(status);
}

void sccb_bus_get_diagnostics (sccb_bus_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        g_diagnostics.transfer_completions = g_transfer_completions;
        g_diagnostics.aborted_events       = g_aborted_events;
        g_diagnostics.unexpected_events    = g_unexpected_events;
        g_diagnostics.last_event           = g_last_event;
        g_diagnostics.transfer_in_progress = g_transfer_in_progress;
        *p_diagnostics                     = g_diagnostics;
    }
}

void camera_i2c0_callback (i2c_master_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        g_unexpected_events++;
        return;
    }

    g_last_event = p_args->event;

    if (I2C_MASTER_EVENT_ABORTED == p_args->event)
    {
        g_aborted_events++;
        if (g_transfer_in_progress)
        {
            g_transfer_aborted = true;
        }
        else
        {
            g_unexpected_events++;
        }

        return;
    }

    if ((I2C_MASTER_EVENT_TX_COMPLETE == p_args->event) ||
        (I2C_MASTER_EVENT_RX_COMPLETE == p_args->event))
    {
        g_transfer_completions++;
        if (g_transfer_in_progress && (g_expected_event == p_args->event))
        {
            g_transfer_complete = true;
        }
        else
        {
            g_unexpected_events++;
        }

        return;
    }

    g_unexpected_events++;
}

static app_status_t sccb_transfer (sccb_transfer_direction_t direction,
                                   uint8_t * p_data,
                                   uint32_t length,
                                   bool restart,
                                   uint32_t timeout_us,
                                   bool * p_retryable)
{
    fsp_err_t err;
    uint32_t  elapsed_us = 0U;

    *p_retryable          = false;
    g_transfer_complete   = false;
    g_transfer_aborted    = false;
    g_transfer_in_progress = true;
    g_expected_event = (SCCB_TRANSFER_READ == direction) ?
                       I2C_MASTER_EVENT_RX_COMPLETE :
                       I2C_MASTER_EVENT_TX_COMPLETE;
    g_diagnostics.transfer_requests++;

    if (SCCB_TRANSFER_READ == direction)
    {
        err = g_i2c_master0.p_api->read(g_i2c_master0.p_ctrl, p_data, length, restart);
    }
    else
    {
        err = g_i2c_master0.p_api->write(g_i2c_master0.p_ctrl, p_data, length, restart);
    }

    if (FSP_SUCCESS != err)
    {
        g_diagnostics.api_errors++;
        g_diagnostics.last_fsp_error = err;
        *p_retryable = (FSP_ERR_IN_USE == err);
        if (*p_retryable)
        {
            sccb_abort();
        }

        g_transfer_in_progress = false;
        return (FSP_ERR_IN_USE == err) ? APP_STATUS_BUSY : APP_STATUS_IO;
    }

    while ((!g_transfer_complete) && (!g_transfer_aborted) && (elapsed_us < timeout_us))
    {
        uint32_t const remaining_us = timeout_us - elapsed_us;
        uint32_t const wait_us = (remaining_us < SCCB_BUS_WAIT_STEP_US) ?
                                 remaining_us :
                                 SCCB_BUS_WAIT_STEP_US;

        R_BSP_SoftwareDelay(wait_us, BSP_DELAY_UNITS_MICROSECONDS);
        elapsed_us += wait_us;
    }

    if (g_transfer_complete)
    {
        g_transfer_in_progress = false;
        return APP_STATUS_OK;
    }

    *p_retryable = true;
    if (g_transfer_aborted)
    {
        g_transfer_in_progress = false;
        return APP_STATUS_IO;
    }

    g_diagnostics.timeouts++;
    sccb_abort();
    g_transfer_in_progress = false;

    return APP_STATUS_TIMEOUT;
}

static void sccb_abort (void)
{
    fsp_err_t err;

    g_diagnostics.abort_requests++;
    err = g_i2c_master0.p_api->abort(g_i2c_master0.p_ctrl);
    g_diagnostics.last_abort_error = err;
    if (FSP_SUCCESS != err)
    {
        g_diagnostics.abort_errors++;
        g_diagnostics.last_fsp_error = err;
    }
}

static app_status_t sccb_record_status (app_status_t status)
{
    g_diagnostics.last_status = status;

    return status;
}

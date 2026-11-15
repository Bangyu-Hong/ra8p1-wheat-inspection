#include "environment_service.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "hal_data.h"
#include "platform_time.h"

#define ENVIRONMENT_DHT11_PIN                 BSP_IO_PORT_00_PIN_10
#define ENVIRONMENT_DHT11_IRQ_CHANNEL         (14U)
#define ENVIRONMENT_LIGHT_ADC_CHANNEL          ADC_CHANNEL_16
#define ENVIRONMENT_LIGHT_CALIBRATION_TIMEOUT_US (100000ULL)
#define ENVIRONMENT_DHT11_RELEASED_CFG         \
    ((uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT | \
     (uint32_t) IOPORT_CFG_PULLUP_ENABLE)
#define ENVIRONMENT_DHT11_CAPTURE_CFG          \
    (ENVIRONMENT_DHT11_RELEASED_CFG | (uint32_t) IOPORT_CFG_IRQ_ENABLE)
#define ENVIRONMENT_DHT11_DRIVE_LOW_CFG        \
    ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
     (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW | \
     (uint32_t) IOPORT_CFG_NMOS_ENABLE)

typedef struct st_environment_service
{
    sensor_dht11_t dht11;
    sensor_light_t light;
    environment_service_hardware_diagnostics_t hardware;
    volatile uint32_t pending_adc_events;
    volatile uint32_t pending_adc_overflows;
    volatile int32_t pending_adc_status;
    volatile int32_t pending_calibration_status;
    volatile bool light_scan_active;
    volatile bool light_calibration_active;
    volatile bool light_calibration_event_pending;
    uint64_t light_calibration_deadline_us;
    bool initialized;
    bool dht11_available;
    bool dht11_irq_open;
    bool dht11_irq_enabled;
    bool light_available;
    bool light_adc_open;
} environment_service_t;

static environment_service_t g_environment;

static void environment_increment(uint32_t * p_value);
static void environment_increment_from_isr(volatile uint32_t * p_value);
static app_status_t environment_status_from_fsp(fsp_err_t err);
static void environment_record_process_error(app_status_t status);
static app_status_t environment_dht11_drive_low(void * p_context);
static app_status_t environment_dht11_release_and_arm(void * p_context);
static app_status_t environment_dht11_disarm(void * p_context);
static app_status_t environment_dht11_elapsed(void * p_context,
                                               uint32_t start_raw_cycles,
                                               uint32_t end_raw_cycles,
                                               uint32_t * p_elapsed_us);
static app_status_t environment_light_start(void * p_context);
static app_status_t environment_light_read(void * p_context, uint16_t * p_raw);
static app_status_t environment_light_stop(void * p_context);
static app_status_t environment_light_initialize_sensor(void);
static app_status_t environment_light_process_calibration(uint64_t now_us);
static void environment_light_close_adc(void);
static void environment_forward_adc_event(void);
static void environment_capture_adc_event_from_isr(app_status_t status);
static void environment_capture_calibration_event_from_isr(app_status_t status);

void environment_dht11_irq_callback(external_irq_callback_args_t * p_args);
void environment_light_adc_callback(adc_callback_args_t * p_args);

app_status_t environment_service_init (uint64_t now_us)
{
    sensor_dht11_config_t dht11_config;
    sensor_dht11_ops_t const dht11_ops =
    {
        .drive_low = environment_dht11_drive_low,
        .release_and_arm_capture = environment_dht11_release_and_arm,
        .disarm_capture = environment_dht11_disarm,
        .elapsed_us = environment_dht11_elapsed,
    };
    fsp_err_t err;
    app_status_t status;

    if (g_environment.initialized)
    {
        return APP_STATUS_OK;
    }

    (void) memset(&g_environment, 0, sizeof(g_environment));
    g_environment.hardware.last_request_status = APP_STATUS_UNAVAILABLE;
    g_environment.hardware.last_process_status = APP_STATUS_OK;
    g_environment.hardware.dht11_init_status = APP_STATUS_UNAVAILABLE;
    g_environment.hardware.light_init_status = APP_STATUS_UNAVAILABLE;
    g_environment.initialized = true;

    err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                 ENVIRONMENT_DHT11_PIN,
                                 ENVIRONMENT_DHT11_RELEASED_CFG);
    status = environment_status_from_fsp(err);
    if (APP_STATUS_OK == status)
    {
        err = g_dht11_irq.p_api->open(g_dht11_irq.p_ctrl, g_dht11_irq.p_cfg);
        status = environment_status_from_fsp(err);
        if (APP_STATUS_OK == status)
        {
            g_environment.dht11_irq_open = true;
            sensor_dht11_config_default(&dht11_config);
            status = sensor_dht11_init(&g_environment.dht11,
                                       &dht11_config,
                                       &dht11_ops,
                                       &g_environment);
            if (APP_STATUS_OK == status)
            {
                g_environment.dht11_available = true;
            }
            else
            {
                (void) g_dht11_irq.p_api->close(g_dht11_irq.p_ctrl);
                g_environment.dht11_irq_open = false;
            }
        }
    }
    g_environment.hardware.dht11_init_status = status;

    err = g_light_adc.p_api->open(g_light_adc.p_ctrl, g_light_adc.p_cfg);
    status = environment_status_from_fsp(err);
    if (APP_STATUS_OK == status)
    {
        g_environment.light_adc_open = true;
        err = g_light_adc.p_api->scanCfg(g_light_adc.p_ctrl,
                                         g_light_adc.p_channel_cfg);
        status = environment_status_from_fsp(err);
        if (APP_STATUS_OK == status)
        {
            __atomic_store_n(&g_environment.pending_calibration_status,
                             (int32_t) APP_STATUS_BUSY,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&g_environment.light_calibration_event_pending,
                             false,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&g_environment.light_calibration_active,
                             true,
                             __ATOMIC_RELEASE);
            err = g_light_adc.p_api->calibrate(g_light_adc.p_ctrl, NULL);
            status = environment_status_from_fsp(err);
            if (APP_STATUS_OK == status)
            {
                g_environment.light_calibration_deadline_us =
                    now_us + ENVIRONMENT_LIGHT_CALIBRATION_TIMEOUT_US;
                status = APP_STATUS_BUSY;
            }
            else
            {
                __atomic_store_n(&g_environment.light_calibration_active,
                                 false,
                                 __ATOMIC_RELEASE);
            }
        }
        if (APP_STATUS_OK != status)
        {
            if (APP_STATUS_BUSY != status)
            {
                environment_light_close_adc();
            }
        }
    }
    g_environment.hardware.light_init_status = status;

    return (g_environment.dht11_available || g_environment.light_available ||
            __atomic_load_n(&g_environment.light_calibration_active,
                            __ATOMIC_ACQUIRE)) ?
           APP_STATUS_OK : APP_STATUS_UNAVAILABLE;
}

app_status_t environment_service_request_dht11 (uint64_t now_us)
{
    app_status_t status;

    if (!g_environment.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if (!g_environment.dht11_available)
    {
        return APP_STATUS_UNAVAILABLE;
    }

    environment_increment(&g_environment.hardware.requests);
    status = sensor_dht11_request(&g_environment.dht11, now_us);
    if (APP_STATUS_BUSY == status)
    {
        environment_increment(&g_environment.hardware.request_busy);
    }
    g_environment.hardware.last_request_status = status;
    return status;
}

app_status_t environment_service_request_light (uint64_t now_us)
{
    app_status_t status;

    if (!g_environment.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if (!g_environment.light_available)
    {
        return APP_STATUS_UNAVAILABLE;
    }

    environment_increment(&g_environment.hardware.requests);
    status = sensor_light_request(&g_environment.light, now_us);
    if (APP_STATUS_BUSY == status)
    {
        environment_increment(&g_environment.hardware.request_busy);
    }
    g_environment.hardware.last_request_status = status;
    return status;
}

app_status_t environment_service_process (uint64_t now_us)
{
    app_status_t dht11_status = APP_STATUS_OK;
    app_status_t light_status = APP_STATUS_OK;
    app_status_t calibration_status = APP_STATUS_OK;
    app_status_t result = APP_STATUS_OK;

    if (!g_environment.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    calibration_status = environment_light_process_calibration(now_us);
    if (g_environment.light_available)
    {
        environment_forward_adc_event();
    }
    if (g_environment.dht11_available)
    {
        dht11_status = sensor_dht11_process(&g_environment.dht11, now_us);
    }
    if (g_environment.light_available)
    {
        light_status = sensor_light_process(&g_environment.light, now_us);
    }
    if (g_environment.light_available)
    {
        environment_forward_adc_event();
    }

    if ((APP_STATUS_OK != calibration_status) &&
        (APP_STATUS_BUSY != calibration_status))
    {
        result = calibration_status;
    }
    if ((APP_STATUS_OK != dht11_status) && (APP_STATUS_BUSY != dht11_status) &&
        (APP_STATUS_OK == result))
    {
        result = dht11_status;
    }
    if ((APP_STATUS_OK != light_status) && (APP_STATUS_BUSY != light_status) &&
        (APP_STATUS_OK == result))
    {
        result = light_status;
    }
    if ((APP_STATUS_BUSY == calibration_status) && (APP_STATUS_OK == result))
    {
        result = APP_STATUS_BUSY;
    }
    if (APP_STATUS_OK != result)
    {
        environment_record_process_error(result);
    }
    g_environment.hardware.last_process_status = result;

    return result;
}

app_status_t environment_service_get_status (
    uint64_t now_us,
    environment_service_status_t * p_status)
{
    if (NULL == p_status)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    (void) memset(p_status, 0, sizeof(*p_status));
    p_status->initialized = g_environment.initialized;
    p_status->dht11_available = g_environment.dht11_available;
    p_status->light_available = g_environment.light_available;
    p_status->hardware = g_environment.hardware;
    if (!g_environment.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    if (g_environment.dht11_available)
    {
        (void) sensor_dht11_get_status(&g_environment.dht11,
                                       &p_status->dht11_state,
                                       &p_status->dht11_fault);
        (void) sensor_dht11_get_reading(&g_environment.dht11,
                                        now_us,
                                        &p_status->dht11_reading);
        sensor_dht11_get_diagnostics(&g_environment.dht11,
                                     &p_status->dht11_diagnostics);
    }
    if (g_environment.light_available)
    {
        (void) sensor_light_get_status(&g_environment.light,
                                       &p_status->light_state,
                                       &p_status->light_fault);
        (void) sensor_light_get_reading(&g_environment.light,
                                        now_us,
                                        &p_status->light_reading);
        sensor_light_get_diagnostics(&g_environment.light,
                                     &p_status->light_diagnostics);
    }

    return (g_environment.dht11_available || g_environment.light_available ||
            __atomic_load_n(&g_environment.light_calibration_active,
                            __ATOMIC_ACQUIRE)) ?
           APP_STATUS_OK : APP_STATUS_UNAVAILABLE;
}

app_status_t environment_service_close (void)
{
    app_status_t result = APP_STATUS_OK;
    app_status_t status;
    fsp_err_t err;

    if (!g_environment.initialized)
    {
        return APP_STATUS_OK;
    }

    g_environment.initialized = false;
    if (g_environment.dht11_available)
    {
        status = sensor_dht11_close(&g_environment.dht11);
        if (APP_STATUS_OK != status)
        {
            result = status;
        }
    }
    if (g_environment.dht11_irq_open)
    {
        err = g_dht11_irq.p_api->disable(g_dht11_irq.p_ctrl);
        if ((FSP_SUCCESS != err) && (APP_STATUS_OK == result))
        {
            result = APP_STATUS_IO;
        }
        err = g_dht11_irq.p_api->close(g_dht11_irq.p_ctrl);
        if ((FSP_SUCCESS != err) && (APP_STATUS_OK == result))
        {
            result = APP_STATUS_IO;
        }
    }
    (void) g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                  ENVIRONMENT_DHT11_PIN,
                                  ENVIRONMENT_DHT11_RELEASED_CFG);

    if (g_environment.light_available)
    {
        status = sensor_light_close(&g_environment.light);
        if ((APP_STATUS_OK != status) && (APP_STATUS_OK == result))
        {
            result = status;
        }
    }
    if (g_environment.light_adc_open)
    {
        err = g_light_adc.p_api->close(g_light_adc.p_ctrl);
        if ((FSP_SUCCESS != err) && (APP_STATUS_OK == result))
        {
            result = APP_STATUS_IO;
        }
    }

    g_environment.dht11_available = false;
    g_environment.light_available = false;
    g_environment.dht11_irq_open = false;
    g_environment.dht11_irq_enabled = false;
    g_environment.light_adc_open = false;
    __atomic_store_n(&g_environment.light_scan_active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_environment.light_calibration_active, false,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_environment.light_calibration_event_pending, false,
                     __ATOMIC_RELEASE);

    return result;
}

void environment_dht11_irq_callback (external_irq_callback_args_t * p_args)
{
    bsp_io_level_t level = BSP_IO_LEVEL_HIGH;
    uint32_t const raw_cycles = platform_time_raw_cycles();
    fsp_err_t err;

    if ((NULL == p_args) ||
        (ENVIRONMENT_DHT11_IRQ_CHANNEL != p_args->channel) ||
        !g_environment.initialized || !g_environment.dht11_available)
    {
        return;
    }

    err = g_ioport.p_api->pinRead(g_ioport.p_ctrl,
                                  ENVIRONMENT_DHT11_PIN,
                                  &level);
    if (FSP_SUCCESS != err)
    {
        environment_increment_from_isr(
            &g_environment.hardware.dht11_pin_read_errors);
        return;
    }

    sensor_dht11_on_edge_isr(&g_environment.dht11,
                             raw_cycles,
                             BSP_IO_LEVEL_HIGH == level);
}

void environment_light_adc_callback (adc_callback_args_t * p_args)
{
    app_status_t status;

    if ((NULL == p_args) || !g_environment.initialized ||
        !g_environment.light_adc_open)
    {
        return;
    }

    if (__atomic_exchange_n(&g_environment.light_calibration_active,
                            false,
                            __ATOMIC_ACQ_REL))
    {
        status = (ADC_EVENT_CALIBRATION_COMPLETE == p_args->event) ?
                 APP_STATUS_OK : APP_STATUS_IO;
        if (APP_STATUS_OK != status)
        {
            environment_increment_from_isr(
                &g_environment.hardware.light_callback_errors);
        }
        environment_capture_calibration_event_from_isr(status);
        return;
    }

    if (!__atomic_exchange_n(&g_environment.light_scan_active,
                             false,
                             __ATOMIC_ACQ_REL))
    {
        environment_increment_from_isr(
            &g_environment.hardware.light_ignored_callbacks);
        return;
    }

    switch (p_args->event)
    {
        case ADC_EVENT_SCAN_COMPLETE:
        {
            status = APP_STATUS_OK;
            break;
        }

        case ADC_EVENT_CONVERSION_ERROR:
        case ADC_EVENT_OVERFLOW:
        case ADC_EVENT_FIFO_OVERFLOW:
        case ADC_EVENT_LIMIT_CLIP:
        {
            environment_increment_from_isr(
                &g_environment.hardware.light_callback_errors);
            status = APP_STATUS_IO;
            break;
        }

        default:
        {
            environment_increment_from_isr(
                &g_environment.hardware.light_ignored_callbacks);
            return;
        }
    }

    environment_capture_adc_event_from_isr(status);
}

static app_status_t environment_dht11_drive_low (void * p_context)
{
    environment_service_t * p_service = (environment_service_t *) p_context;
    fsp_err_t err;

    if ((NULL == p_service) || !p_service->dht11_irq_open)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if (p_service->dht11_irq_enabled)
    {
        err = g_dht11_irq.p_api->disable(g_dht11_irq.p_ctrl);
        if (FSP_SUCCESS != err)
        {
            return APP_STATUS_IO;
        }
        p_service->dht11_irq_enabled = false;
    }

    err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                 ENVIRONMENT_DHT11_PIN,
                                 ENVIRONMENT_DHT11_DRIVE_LOW_CFG);
    return environment_status_from_fsp(err);
}

static app_status_t environment_dht11_release_and_arm (void * p_context)
{
    environment_service_t * p_service = (environment_service_t *) p_context;
    fsp_err_t err;

    if ((NULL == p_service) || !p_service->dht11_irq_open)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                 ENVIRONMENT_DHT11_PIN,
                                 ENVIRONMENT_DHT11_CAPTURE_CFG);
    if (FSP_SUCCESS != err)
    {
        return APP_STATUS_IO;
    }
    err = g_dht11_irq.p_api->enable(g_dht11_irq.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        (void) g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                      ENVIRONMENT_DHT11_PIN,
                                      ENVIRONMENT_DHT11_RELEASED_CFG);
        return APP_STATUS_IO;
    }
    p_service->dht11_irq_enabled = true;

    return APP_STATUS_OK;
}

static app_status_t environment_dht11_disarm (void * p_context)
{
    environment_service_t * p_service = (environment_service_t *) p_context;
    app_status_t result = APP_STATUS_OK;
    fsp_err_t err;

    if ((NULL == p_service) || !p_service->dht11_irq_open)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if (p_service->dht11_irq_enabled)
    {
        err = g_dht11_irq.p_api->disable(g_dht11_irq.p_ctrl);
        if (FSP_SUCCESS != err)
        {
            result = APP_STATUS_IO;
        }
        p_service->dht11_irq_enabled = false;
    }
    err = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                 ENVIRONMENT_DHT11_PIN,
                                 ENVIRONMENT_DHT11_RELEASED_CFG);
    if (FSP_SUCCESS != err)
    {
        result = APP_STATUS_IO;
    }

    return result;
}

static app_status_t environment_dht11_elapsed (void * p_context,
                                                uint32_t start_raw_cycles,
                                                uint32_t end_raw_cycles,
                                                uint32_t * p_elapsed_us)
{
    (void) p_context;
    return platform_time_elapsed_us(start_raw_cycles,
                                    end_raw_cycles,
                                    p_elapsed_us);
}

static app_status_t environment_light_start (void * p_context)
{
    environment_service_t * p_service = (environment_service_t *) p_context;
    fsp_err_t err;

    if ((NULL == p_service) || !p_service->light_adc_open)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    __atomic_store_n(&p_service->light_scan_active, true, __ATOMIC_RELEASE);
    err = g_light_adc.p_api->scanStart(g_light_adc.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        __atomic_store_n(&p_service->light_scan_active, false, __ATOMIC_RELEASE);
    }

    return environment_status_from_fsp(err);
}

static app_status_t environment_light_read (void * p_context, uint16_t * p_raw)
{
    environment_service_t * p_service = (environment_service_t *) p_context;
    fsp_err_t err;

    if ((NULL == p_service) || !p_service->light_adc_open)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if (NULL == p_raw)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    err = g_light_adc.p_api->read(g_light_adc.p_ctrl,
                                  ENVIRONMENT_LIGHT_ADC_CHANNEL,
                                  p_raw);
    return environment_status_from_fsp(err);
}

static app_status_t environment_light_stop (void * p_context)
{
    environment_service_t * p_service = (environment_service_t *) p_context;
    bool was_active;
    fsp_err_t err;

    if ((NULL == p_service) || !p_service->light_adc_open)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    was_active = __atomic_exchange_n(&p_service->light_scan_active,
                                     false,
                                     __ATOMIC_ACQ_REL);
    if (!was_active)
    {
        return APP_STATUS_OK;
    }

    err = g_light_adc.p_api->scanStop(g_light_adc.p_ctrl);
    return environment_status_from_fsp(err);
}

static app_status_t environment_light_initialize_sensor (void)
{
    sensor_light_config_t light_config;
    sensor_light_ops_t const light_ops =
    {
        .start_sample = environment_light_start,
        .read_sample = environment_light_read,
        .stop = environment_light_stop,
    };
    app_status_t status;

    sensor_light_config_default(&light_config);
    status = sensor_light_init(&g_environment.light,
                               &light_config,
                               &light_ops,
                               &g_environment);
    if (APP_STATUS_OK == status)
    {
        g_environment.light_available = true;
        environment_increment(
            &g_environment.hardware.light_calibrations_completed);
    }

    g_environment.hardware.light_init_status = status;
    return status;
}

static app_status_t environment_light_process_calibration (uint64_t now_us)
{
    bool const event_pending = __atomic_exchange_n(
        &g_environment.light_calibration_event_pending,
        false,
        __ATOMIC_ACQ_REL);
    app_status_t status;

    if (event_pending)
    {
        status = (app_status_t) __atomic_load_n(
            &g_environment.pending_calibration_status,
            __ATOMIC_ACQUIRE);
        if (APP_STATUS_OK == status)
        {
            status = environment_light_initialize_sensor();
        }
        if (APP_STATUS_OK != status)
        {
            environment_increment(
                &g_environment.hardware.light_calibration_errors);
            g_environment.hardware.light_init_status = status;
            environment_light_close_adc();
        }
        return status;
    }

    if (__atomic_load_n(&g_environment.light_calibration_active,
                        __ATOMIC_ACQUIRE))
    {
        if (now_us < g_environment.light_calibration_deadline_us)
        {
            return APP_STATUS_BUSY;
        }
        if (__atomic_exchange_n(&g_environment.light_calibration_active,
                                false,
                                __ATOMIC_ACQ_REL))
        {
            environment_increment(
                &g_environment.hardware.light_calibration_timeouts);
            g_environment.hardware.light_init_status = APP_STATUS_TIMEOUT;
            environment_light_close_adc();
            return APP_STATUS_TIMEOUT;
        }
    }

    /* Calibration and initialization failures are reported by the branch
     * that observes them.  Keep light_init_status for diagnostics, but do
     * not replay the same permanent offline error on every cooperative poll. */
    return APP_STATUS_OK;
}

static void environment_light_close_adc (void)
{
    if (g_environment.light_adc_open)
    {
        (void) g_light_adc.p_api->close(g_light_adc.p_ctrl);
        g_environment.light_adc_open = false;
    }
    g_environment.light_available = false;
    __atomic_store_n(&g_environment.light_scan_active, false, __ATOMIC_RELEASE);
}

static void environment_forward_adc_event (void)
{
    uint32_t const pending = __atomic_exchange_n(
        &g_environment.pending_adc_events, 0U, __ATOMIC_ACQ_REL);
    uint32_t const overflows = __atomic_exchange_n(
        &g_environment.pending_adc_overflows, 0U, __ATOMIC_ACQ_REL);
    app_status_t status;

    if (0U == pending)
    {
        return;
    }

    status = (0U < overflows) ? APP_STATUS_DATA :
             (app_status_t) __atomic_load_n(&g_environment.pending_adc_status,
                                             __ATOMIC_ACQUIRE);
    sensor_light_on_adc_event_isr(&g_environment.light, status);
}

static void environment_capture_adc_event_from_isr (app_status_t status)
{
    uint32_t expected = 0U;

    __atomic_store_n(&g_environment.pending_adc_status,
                     (int32_t) status,
                     __ATOMIC_RELAXED);
    if (!__atomic_compare_exchange_n(&g_environment.pending_adc_events,
                                     &expected,
                                     1U,
                                     false,
                                     __ATOMIC_RELEASE,
                                     __ATOMIC_RELAXED))
    {
        environment_increment_from_isr(&g_environment.pending_adc_overflows);
        environment_increment_from_isr(
            &g_environment.hardware.light_callback_overflows);
    }
}

static void environment_capture_calibration_event_from_isr (app_status_t status)
{
    __atomic_store_n(&g_environment.pending_calibration_status,
                     (int32_t) status,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_environment.light_calibration_event_pending,
                     true,
                     __ATOMIC_RELEASE);
}

static app_status_t environment_status_from_fsp (fsp_err_t err)
{
    return (FSP_SUCCESS == err) ? APP_STATUS_OK : APP_STATUS_IO;
}

static void environment_record_process_error (app_status_t status)
{
    if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
    {
        environment_increment(&g_environment.hardware.process_errors);
    }
}

static void environment_increment (uint32_t * p_value)
{
    if ((NULL != p_value) && (UINT32_MAX != *p_value))
    {
        (*p_value)++;
    }
}

static void environment_increment_from_isr (volatile uint32_t * p_value)
{
    uint32_t current = __atomic_load_n(p_value, __ATOMIC_RELAXED);

    while (UINT32_MAX != current)
    {
        uint32_t const desired = current + 1U;

        if (__atomic_compare_exchange_n(p_value,
                                        &current,
                                        desired,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
        {
            break;
        }
    }
}

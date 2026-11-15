#include "sensor_light.h"

#include <limits.h>
#include <stddef.h>

#define SENSOR_LIGHT_DEFAULT_ADC_MAX_RAW          (4095U)
#define SENSOR_LIGHT_DEFAULT_CALIBRATION_VERSION  (1U)
#define SENSOR_LIGHT_DEFAULT_SAMPLE_TIMEOUT_US    (10000U)
#define SENSOR_LIGHT_DEFAULT_BACKOFF_US           (250000U)
#define SENSOR_LIGHT_DEFAULT_STALE_AFTER_US       (5000000ULL)

static void sensor_light_increment(uint32_t * p_value);
static void sensor_light_add(uint32_t * p_value, uint32_t amount);
static void sensor_light_increment_from_isr(volatile uint32_t * p_value);
static bool sensor_light_config_valid(sensor_light_config_t const * p_config);
static bool sensor_light_ops_valid(sensor_light_ops_t const * p_ops);
static app_status_t sensor_light_enter_error(sensor_light_t * p_sensor,
                                             sensor_light_fault_t fault,
                                             app_status_t status);
static app_status_t sensor_light_finish_cycle(sensor_light_t * p_sensor,
                                              uint64_t now_us);
static void sensor_light_update_stale(sensor_light_t * p_sensor,
                                      uint64_t now_us);
static uint32_t sensor_light_age_ms(uint64_t now_us, uint64_t sample_time_us);

void sensor_light_config_default (sensor_light_config_t * p_config)
{
    if (NULL == p_config)
    {
        return;
    }

    *p_config = (sensor_light_config_t)
    {
        .adc_max_raw = SENSOR_LIGHT_DEFAULT_ADC_MAX_RAW,
        .calibration_dark_raw = 0U,
        .calibration_bright_raw = SENSOR_LIGHT_DEFAULT_ADC_MAX_RAW,
        .calibration_version = SENSOR_LIGHT_DEFAULT_CALIBRATION_VERSION,
        .sample_timeout_us = SENSOR_LIGHT_DEFAULT_SAMPLE_TIMEOUT_US,
        .backoff_us = SENSOR_LIGHT_DEFAULT_BACKOFF_US,
        .minimum_cycle_interval_us = SENSOR_LIGHT_MIN_CYCLE_INTERVAL_US,
        .stale_after_us = SENSOR_LIGHT_DEFAULT_STALE_AFTER_US,
        .reverse_percent = false,
    };
}

app_status_t sensor_light_init (sensor_light_t * p_sensor,
                                sensor_light_config_t const * p_config,
                                sensor_light_ops_t const * p_ops,
                                void * p_context)
{
    if ((NULL == p_sensor) || !sensor_light_config_valid(p_config) ||
        !sensor_light_ops_valid(p_ops))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_sensor = (sensor_light_t) {0};
    p_sensor->config = *p_config;
    p_sensor->ops = *p_ops;
    p_sensor->p_context = p_context;
    p_sensor->state = SENSOR_LIGHT_STATE_IDLE;
    p_sensor->diagnostics.last_status = APP_STATUS_OK;
    p_sensor->diagnostics.last_fault = SENSOR_LIGHT_FAULT_NONE;
    p_sensor->initialized = true;

    return APP_STATUS_OK;
}

app_status_t sensor_light_request (sensor_light_t * p_sensor, uint64_t now_us)
{
    if ((NULL == p_sensor) || !p_sensor->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if ((SENSOR_LIGHT_STATE_IDLE != p_sensor->state) &&
        (SENSOR_LIGHT_STATE_READY != p_sensor->state))
    {
        return APP_STATUS_BUSY;
    }
    if (now_us < p_sensor->next_request_us)
    {
        sensor_light_increment(&p_sensor->diagnostics.rate_limited_requests);
        return APP_STATUS_BUSY;
    }

    sensor_light_increment(&p_sensor->diagnostics.cycles_requested);
    p_sensor->sample_count = 0U;
    __atomic_store_n(&p_sensor->sample_in_flight, false, __ATOMIC_RELEASE);
    p_sensor->reading.current_cycle_valid = false;
    p_sensor->diagnostics.last_status = APP_STATUS_BUSY;
    p_sensor->diagnostics.last_fault = SENSOR_LIGHT_FAULT_NONE;
    p_sensor->next_request_us = now_us + p_sensor->config.minimum_cycle_interval_us;
    __atomic_store_n(&p_sensor->pending_events, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&p_sensor->event_overflow_events, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&p_sensor->pending_status, (int32_t) APP_STATUS_OK,
                     __ATOMIC_RELAXED);
    p_sensor->state = SENSOR_LIGHT_STATE_SAMPLING;

    return APP_STATUS_OK;
}

app_status_t sensor_light_process (sensor_light_t * p_sensor, uint64_t now_us)
{
    uint32_t pending_events;
    uint32_t overflow_events;
    app_status_t event_status;
    app_status_t status;
    uint16_t raw = 0U;

    if ((NULL == p_sensor) || !p_sensor->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    sensor_light_update_stale(p_sensor, now_us);

    switch (p_sensor->state)
    {
        case SENSOR_LIGHT_STATE_IDLE:
        case SENSOR_LIGHT_STATE_READY:
        {
            return APP_STATUS_OK;
        }

        case SENSOR_LIGHT_STATE_SAMPLING:
        {
            pending_events = __atomic_exchange_n(&p_sensor->pending_events, 0U,
                                                  __ATOMIC_ACQ_REL);
            overflow_events = __atomic_exchange_n(&p_sensor->event_overflow_events,
                                                   0U,
                                                   __ATOMIC_ACQ_REL);
            if (0U < pending_events)
            {
                __atomic_store_n(&p_sensor->sample_in_flight, false,
                                 __ATOMIC_RELEASE);
            }
            if (0U < overflow_events)
            {
                sensor_light_add(&p_sensor->diagnostics.event_overflows,
                                 overflow_events);
                return sensor_light_enter_error(p_sensor,
                                                SENSOR_LIGHT_FAULT_EVENT_OVERFLOW,
                                                APP_STATUS_DATA);
            }
            if (1U == pending_events)
            {
                event_status = (app_status_t) __atomic_load_n(&p_sensor->pending_status,
                                                               __ATOMIC_ACQUIRE);
                if (APP_STATUS_OK != event_status)
                {
                    sensor_light_increment(&p_sensor->diagnostics.adc_errors);
                    return sensor_light_enter_error(p_sensor,
                                                    SENSOR_LIGHT_FAULT_ADC,
                                                    event_status);
                }

                status = p_sensor->ops.read_sample(p_sensor->p_context, &raw);
                if (APP_STATUS_OK != status)
                {
                    sensor_light_increment(&p_sensor->diagnostics.adc_errors);
                    return sensor_light_enter_error(p_sensor,
                                                    SENSOR_LIGHT_FAULT_ADC,
                                                    status);
                }
                if (raw > p_sensor->config.adc_max_raw)
                {
                    sensor_light_increment(&p_sensor->diagnostics.range_errors);
                    return sensor_light_enter_error(p_sensor,
                                                    SENSOR_LIGHT_FAULT_RANGE,
                                                    APP_STATUS_DATA);
                }

                p_sensor->samples[p_sensor->sample_count] = raw;
                p_sensor->sample_count++;
                sensor_light_increment(&p_sensor->diagnostics.samples_completed);
                if (p_sensor->sample_count >= SENSOR_LIGHT_SAMPLES_PER_CYCLE)
                {
                    return sensor_light_finish_cycle(p_sensor, now_us);
                }
            }

            if (__atomic_load_n(&p_sensor->sample_in_flight, __ATOMIC_ACQUIRE))
            {
                if (now_us >= p_sensor->deadline_us)
                {
                    sensor_light_increment(&p_sensor->diagnostics.timeouts);
                    return sensor_light_enter_error(p_sensor,
                                                    SENSOR_LIGHT_FAULT_TIMEOUT,
                                                    APP_STATUS_TIMEOUT);
                }
                return APP_STATUS_BUSY;
            }

            status = p_sensor->ops.start_sample(p_sensor->p_context);
            if (APP_STATUS_OK != status)
            {
                sensor_light_increment(&p_sensor->diagnostics.hardware_errors);
                return sensor_light_enter_error(p_sensor,
                                                SENSOR_LIGHT_FAULT_HARDWARE,
                                                status);
            }
            sensor_light_increment(&p_sensor->diagnostics.sample_requests);
            __atomic_store_n(&p_sensor->sample_in_flight, true,
                             __ATOMIC_RELEASE);
            p_sensor->deadline_us = now_us + p_sensor->config.sample_timeout_us;
            return APP_STATUS_BUSY;
        }

        case SENSOR_LIGHT_STATE_ERROR:
        {
            status = p_sensor->ops.stop(p_sensor->p_context);
            if (APP_STATUS_OK != status)
            {
                sensor_light_increment(&p_sensor->diagnostics.hardware_errors);
                p_sensor->diagnostics.last_status = status;
                p_sensor->diagnostics.last_fault = SENSOR_LIGHT_FAULT_HARDWARE;
            }
            __atomic_store_n(&p_sensor->sample_in_flight, false,
                             __ATOMIC_RELEASE);
            p_sensor->deadline_us = now_us + p_sensor->config.backoff_us;
            if (p_sensor->deadline_us < p_sensor->next_request_us)
            {
                p_sensor->deadline_us = p_sensor->next_request_us;
            }
            p_sensor->state = SENSOR_LIGHT_STATE_BACKOFF;
            return p_sensor->diagnostics.last_status;
        }

        case SENSOR_LIGHT_STATE_BACKOFF:
        {
            if (now_us < p_sensor->deadline_us)
            {
                return APP_STATUS_BUSY;
            }
            p_sensor->state = SENSOR_LIGHT_STATE_IDLE;
            return APP_STATUS_OK;
        }

        default:
        {
            return sensor_light_enter_error(p_sensor,
                                            SENSOR_LIGHT_FAULT_HARDWARE,
                                            APP_STATUS_DATA);
        }
    }
}

void sensor_light_on_adc_event_isr (sensor_light_t * p_sensor,
                                    app_status_t event_status)
{
    uint32_t pending;
    uint32_t desired;

    if ((NULL == p_sensor) ||
        !__atomic_load_n(&p_sensor->sample_in_flight, __ATOMIC_ACQUIRE))
    {
        return;
    }

    pending = __atomic_load_n(&p_sensor->pending_events, __ATOMIC_RELAXED);
    do
    {
        if (pending >= 1U)
        {
            sensor_light_increment_from_isr(&p_sensor->event_overflow_events);
            return;
        }
        if (0U == pending)
        {
            __atomic_store_n(&p_sensor->pending_status, (int32_t) event_status,
                             __ATOMIC_RELAXED);
        }
        desired = pending + 1U;
    } while (!__atomic_compare_exchange_n(&p_sensor->pending_events,
                                           &pending,
                                           desired,
                                           false,
                                           __ATOMIC_RELEASE,
                                           __ATOMIC_RELAXED));
}

app_status_t sensor_light_get_reading (sensor_light_t * p_sensor,
                                       uint64_t now_us,
                                       sensor_light_reading_t * p_reading)
{
    if ((NULL == p_sensor) || (NULL == p_reading))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (!p_sensor->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    sensor_light_update_stale(p_sensor, now_us);
    *p_reading = p_sensor->reading;
    p_reading->age_ms = p_reading->has_value ?
                        sensor_light_age_ms(now_us, p_reading->sample_time_us) :
                        UINT32_MAX;

    return p_reading->has_value ? APP_STATUS_OK : APP_STATUS_UNAVAILABLE;
}

app_status_t sensor_light_get_status (sensor_light_t const * p_sensor,
                                      sensor_light_state_t * p_state,
                                      sensor_light_fault_t * p_fault)
{
    if ((NULL == p_sensor) || (NULL == p_state) || (NULL == p_fault))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (!p_sensor->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    *p_state = p_sensor->state;
    *p_fault = p_sensor->diagnostics.last_fault;
    return p_sensor->diagnostics.last_status;
}

void sensor_light_get_diagnostics (sensor_light_t const * p_sensor,
                                   sensor_light_diagnostics_t * p_diagnostics)
{
    if ((NULL != p_sensor) && (NULL != p_diagnostics))
    {
        *p_diagnostics = p_sensor->diagnostics;
    }
}

app_status_t sensor_light_close (sensor_light_t * p_sensor)
{
    app_status_t status;

    if (NULL == p_sensor)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (!p_sensor->initialized)
    {
        return APP_STATUS_OK;
    }

    status = p_sensor->ops.stop(p_sensor->p_context);
    p_sensor->state = SENSOR_LIGHT_STATE_IDLE;
    p_sensor->initialized = false;
    __atomic_store_n(&p_sensor->sample_in_flight, false, __ATOMIC_RELEASE);

    return status;
}

uint16_t sensor_light_calibrate_tenths_percent (
    sensor_light_config_t const * p_config,
    uint16_t raw)
{
    uint32_t scaled;
    uint32_t span;

    if ((NULL == p_config) ||
        (p_config->calibration_bright_raw <= p_config->calibration_dark_raw))
    {
        return 0U;
    }
    span = (uint32_t) p_config->calibration_bright_raw -
           p_config->calibration_dark_raw;
    if (raw <= p_config->calibration_dark_raw)
    {
        scaled = 0U;
    }
    else if (raw >= p_config->calibration_bright_raw)
    {
        scaled = SENSOR_LIGHT_PERCENT_TENTHS_MAX;
    }
    else
    {
        uint32_t const offset = (uint32_t) raw - p_config->calibration_dark_raw;

        scaled = ((offset * SENSOR_LIGHT_PERCENT_TENTHS_MAX) + (span / 2U)) /
                 span;
    }

    if (p_config->reverse_percent)
    {
        scaled = SENSOR_LIGHT_PERCENT_TENTHS_MAX - scaled;
    }

    return (uint16_t) scaled;
}

char const * sensor_light_state_name (sensor_light_state_t state)
{
    static char const * const names[] =
    {
        "idle", "sampling", "ready", "error", "backoff"
    };

    if ((uint32_t) state >= (uint32_t) (sizeof(names) / sizeof(names[0])))
    {
        return "unknown";
    }

    return names[state];
}

char const * sensor_light_fault_name (sensor_light_fault_t fault)
{
    static char const * const names[] =
    {
        "none", "timeout", "adc", "range", "event-overflow", "hardware"
    };

    if ((uint32_t) fault >= (uint32_t) (sizeof(names) / sizeof(names[0])))
    {
        return "unknown";
    }

    return names[fault];
}

static void sensor_light_increment (uint32_t * p_value)
{
    if ((NULL != p_value) && (UINT32_MAX != *p_value))
    {
        (*p_value)++;
    }
}

static void sensor_light_add (uint32_t * p_value, uint32_t amount)
{
    if (NULL != p_value)
    {
        *p_value = (amount > (UINT32_MAX - *p_value)) ?
                   UINT32_MAX : (*p_value + amount);
    }
}

static void sensor_light_increment_from_isr (volatile uint32_t * p_value)
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

static bool sensor_light_config_valid (sensor_light_config_t const * p_config)
{
    return (NULL != p_config) &&
           (0U < p_config->adc_max_raw) &&
           (p_config->calibration_dark_raw < p_config->calibration_bright_raw) &&
           (p_config->calibration_bright_raw <= p_config->adc_max_raw) &&
           (0U < p_config->calibration_version) &&
           (0U < p_config->sample_timeout_us) &&
           (0U < p_config->backoff_us) &&
           (p_config->minimum_cycle_interval_us >= SENSOR_LIGHT_MIN_CYCLE_INTERVAL_US) &&
           (0U < p_config->stale_after_us);
}

static bool sensor_light_ops_valid (sensor_light_ops_t const * p_ops)
{
    return (NULL != p_ops) &&
           (NULL != p_ops->start_sample) &&
           (NULL != p_ops->read_sample) &&
           (NULL != p_ops->stop);
}

static app_status_t sensor_light_enter_error (sensor_light_t * p_sensor,
                                              sensor_light_fault_t fault,
                                              app_status_t status)
{
    p_sensor->reading.current_cycle_valid = false;
    p_sensor->diagnostics.last_fault = fault;
    p_sensor->diagnostics.last_status = status;
    p_sensor->state = SENSOR_LIGHT_STATE_ERROR;

    return status;
}

static app_status_t sensor_light_finish_cycle (sensor_light_t * p_sensor,
                                               uint64_t now_us)
{
    uint32_t index;
    uint32_t sum = 0U;
    uint16_t minimum = UINT16_MAX;
    uint16_t maximum = 0U;
    uint16_t average;

    for (index = 0U; index < SENSOR_LIGHT_SAMPLES_PER_CYCLE; index++)
    {
        uint16_t const sample = p_sensor->samples[index];

        sum += sample;
        if (sample < minimum)
        {
            minimum = sample;
        }
        if (sample > maximum)
        {
            maximum = sample;
        }
    }

    sum -= minimum;
    sum -= maximum;
    average = (uint16_t) ((sum + 3U) / 6U);

    p_sensor->reading.raw = p_sensor->samples[SENSOR_LIGHT_SAMPLES_PER_CYCLE - 1U];
    p_sensor->reading.filtered_average = average;
    p_sensor->reading.calibrated_tenths_percent =
        sensor_light_calibrate_tenths_percent(&p_sensor->config, average);
    p_sensor->reading.calibration_version = p_sensor->config.calibration_version;
    p_sensor->reading.sample_time_us = now_us;
    p_sensor->reading.age_ms = 0U;
    p_sensor->reading.has_value = true;
    p_sensor->reading.current_cycle_valid = true;
    p_sensor->reading.stale = false;
    p_sensor->stale_reported = false;
    p_sensor->diagnostics.last_cycle_samples = p_sensor->sample_count;
    if (p_sensor->sample_count > p_sensor->diagnostics.max_cycle_samples)
    {
        p_sensor->diagnostics.max_cycle_samples = p_sensor->sample_count;
    }
    sensor_light_increment(&p_sensor->diagnostics.cycles_completed);
    p_sensor->diagnostics.last_fault = SENSOR_LIGHT_FAULT_NONE;
    p_sensor->diagnostics.last_status = APP_STATUS_OK;
    p_sensor->state = SENSOR_LIGHT_STATE_READY;

    return APP_STATUS_OK;
}

static void sensor_light_update_stale (sensor_light_t * p_sensor,
                                       uint64_t now_us)
{
    bool const stale = p_sensor->reading.has_value &&
                       (now_us >= p_sensor->reading.sample_time_us) &&
                       ((now_us - p_sensor->reading.sample_time_us) >=
                        p_sensor->config.stale_after_us);

    p_sensor->reading.stale = stale;
    if (stale && !p_sensor->stale_reported)
    {
        sensor_light_increment(&p_sensor->diagnostics.stale_events);
        p_sensor->stale_reported = true;
    }
}

static uint32_t sensor_light_age_ms (uint64_t now_us, uint64_t sample_time_us)
{
    uint64_t age_ms;

    if (now_us < sample_time_us)
    {
        return 0U;
    }

    age_ms = (now_us - sample_time_us) / 1000U;
    return (age_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t) age_ms;
}

#include "sensor_dht11.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define SENSOR_DHT11_DEFAULT_START_LOW_US          (18000U)
#define SENSOR_DHT11_DEFAULT_RESPONSE_TIMEOUT_US   (300U)
#define SENSOR_DHT11_DEFAULT_CAPTURE_TIMEOUT_US    (6000U)
#define SENSOR_DHT11_DEFAULT_BACKOFF_US            (250000U)
#define SENSOR_DHT11_DEFAULT_ONE_THRESHOLD_US      (50U)
#define SENSOR_DHT11_DEFAULT_PULSE_MIN_US          (15U)
#define SENSOR_DHT11_DEFAULT_PULSE_MAX_US          (120U)
#define SENSOR_DHT11_DEFAULT_STALE_AFTER_US        (5000000ULL)
#define SENSOR_DHT11_DEFAULT_TEMPERATURE_MIN_MC    (0)
#define SENSOR_DHT11_DEFAULT_TEMPERATURE_MAX_MC    (50000)
#define SENSOR_DHT11_DEFAULT_HUMIDITY_MIN_MPCT     (20000U)
#define SENSOR_DHT11_DEFAULT_HUMIDITY_MAX_MPCT     (90000U)

static void sensor_dht11_increment(uint32_t * p_value);
static void sensor_dht11_add(uint32_t * p_value, uint32_t amount);
static void sensor_dht11_increment_from_isr(volatile uint32_t * p_value);
static bool sensor_dht11_config_valid(sensor_dht11_config_t const * p_config);
static bool sensor_dht11_ops_valid(sensor_dht11_ops_t const * p_ops);
static app_status_t sensor_dht11_finish_capture(sensor_dht11_t * p_sensor);
static app_status_t sensor_dht11_enter_error(sensor_dht11_t * p_sensor,
                                             sensor_dht11_fault_t fault,
                                             app_status_t status);
static app_status_t sensor_dht11_decode(sensor_dht11_t * p_sensor);
static app_status_t sensor_dht11_validate(sensor_dht11_t * p_sensor,
                                          uint64_t now_us);
static void sensor_dht11_update_stale(sensor_dht11_t * p_sensor,
                                      uint64_t now_us);
static uint32_t sensor_dht11_age_ms(uint64_t now_us, uint64_t sample_time_us);

void sensor_dht11_config_default (sensor_dht11_config_t * p_config)
{
    if (NULL == p_config)
    {
        return;
    }

    *p_config = (sensor_dht11_config_t)
    {
        .start_low_us = SENSOR_DHT11_DEFAULT_START_LOW_US,
        .response_timeout_us = SENSOR_DHT11_DEFAULT_RESPONSE_TIMEOUT_US,
        .capture_timeout_us = SENSOR_DHT11_DEFAULT_CAPTURE_TIMEOUT_US,
        .backoff_us = SENSOR_DHT11_DEFAULT_BACKOFF_US,
        .one_threshold_us = SENSOR_DHT11_DEFAULT_ONE_THRESHOLD_US,
        .pulse_min_us = SENSOR_DHT11_DEFAULT_PULSE_MIN_US,
        .pulse_max_us = SENSOR_DHT11_DEFAULT_PULSE_MAX_US,
        .minimum_sample_interval_us = SENSOR_DHT11_MIN_SAMPLE_INTERVAL_US,
        .stale_after_us = SENSOR_DHT11_DEFAULT_STALE_AFTER_US,
        .temperature_min_milli_c = SENSOR_DHT11_DEFAULT_TEMPERATURE_MIN_MC,
        .temperature_max_milli_c = SENSOR_DHT11_DEFAULT_TEMPERATURE_MAX_MC,
        .humidity_min_milli_percent = SENSOR_DHT11_DEFAULT_HUMIDITY_MIN_MPCT,
        .humidity_max_milli_percent = SENSOR_DHT11_DEFAULT_HUMIDITY_MAX_MPCT,
    };
}

app_status_t sensor_dht11_init (sensor_dht11_t * p_sensor,
                                sensor_dht11_config_t const * p_config,
                                sensor_dht11_ops_t const * p_ops,
                                void * p_context)
{
    if ((NULL == p_sensor) || !sensor_dht11_config_valid(p_config) ||
        !sensor_dht11_ops_valid(p_ops))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_sensor = (sensor_dht11_t) {0};
    p_sensor->config = *p_config;
    p_sensor->ops = *p_ops;
    p_sensor->p_context = p_context;
    p_sensor->state = SENSOR_DHT11_STATE_IDLE;
    p_sensor->diagnostics.last_status = APP_STATUS_OK;
    p_sensor->diagnostics.last_fault = SENSOR_DHT11_FAULT_NONE;
    p_sensor->initialized = true;

    return APP_STATUS_OK;
}

app_status_t sensor_dht11_request (sensor_dht11_t * p_sensor, uint64_t now_us)
{
    app_status_t status;

    if ((NULL == p_sensor) || !p_sensor->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if ((SENSOR_DHT11_STATE_IDLE != p_sensor->state) &&
        (SENSOR_DHT11_STATE_READY != p_sensor->state))
    {
        return APP_STATUS_BUSY;
    }
    if (now_us < p_sensor->next_request_us)
    {
        sensor_dht11_increment(&p_sensor->diagnostics.rate_limited_requests);
        return APP_STATUS_BUSY;
    }

    sensor_dht11_increment(&p_sensor->diagnostics.requests);
    p_sensor->next_request_us = now_us + p_sensor->config.minimum_sample_interval_us;
    status = p_sensor->ops.drive_low(p_sensor->p_context);
    if (APP_STATUS_OK != status)
    {
        sensor_dht11_increment(&p_sensor->diagnostics.hardware_errors);
        p_sensor->diagnostics.last_status = status;
        p_sensor->diagnostics.last_fault = SENSOR_DHT11_FAULT_HARDWARE;
        p_sensor->state = SENSOR_DHT11_STATE_ERROR;
        return status;
    }

    p_sensor->reading.current_sample_valid = false;
    p_sensor->diagnostics.last_status = APP_STATUS_BUSY;
    p_sensor->diagnostics.last_fault = SENSOR_DHT11_FAULT_NONE;
    p_sensor->deadline_us = now_us + p_sensor->config.start_low_us;
    p_sensor->state = SENSOR_DHT11_STATE_START_LOW;

    return APP_STATUS_OK;
}

app_status_t sensor_dht11_process (sensor_dht11_t * p_sensor, uint64_t now_us)
{
    uint32_t edge_count;
    uint32_t overflow_events;
    app_status_t status;

    if ((NULL == p_sensor) || !p_sensor->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    sensor_dht11_update_stale(p_sensor, now_us);

    switch (p_sensor->state)
    {
        case SENSOR_DHT11_STATE_IDLE:
        case SENSOR_DHT11_STATE_READY:
        {
            return APP_STATUS_OK;
        }

        case SENSOR_DHT11_STATE_START_LOW:
        {
            if (now_us < p_sensor->deadline_us)
            {
                return APP_STATUS_BUSY;
            }

            __atomic_store_n(&p_sensor->edge_count, 0U, __ATOMIC_RELAXED);
            __atomic_store_n(&p_sensor->edge_overflow_events, 0U, __ATOMIC_RELAXED);
            __atomic_store_n(&p_sensor->capture_enabled, true, __ATOMIC_RELEASE);
            status = p_sensor->ops.release_and_arm_capture(p_sensor->p_context);
            if (APP_STATUS_OK != status)
            {
                __atomic_store_n(&p_sensor->capture_enabled, false, __ATOMIC_RELEASE);
                sensor_dht11_increment(&p_sensor->diagnostics.hardware_errors);
                return sensor_dht11_enter_error(p_sensor,
                                                SENSOR_DHT11_FAULT_HARDWARE,
                                                status);
            }

            p_sensor->capture_armed = true;
            p_sensor->deadline_us = now_us + p_sensor->config.response_timeout_us;
            p_sensor->capture_deadline_us = now_us +
                                            p_sensor->config.capture_timeout_us;
            p_sensor->state = SENSOR_DHT11_STATE_RELEASE;
            return APP_STATUS_BUSY;
        }

        case SENSOR_DHT11_STATE_RELEASE:
        {
            overflow_events = __atomic_load_n(&p_sensor->edge_overflow_events,
                                               __ATOMIC_ACQUIRE);
            if (0U < overflow_events)
            {
                sensor_dht11_increment(&p_sensor->diagnostics.edge_count_errors);
                sensor_dht11_add(&p_sensor->diagnostics.edge_overflows,
                                 overflow_events);
                return sensor_dht11_enter_error(p_sensor,
                                                SENSOR_DHT11_FAULT_EDGE_COUNT,
                                                APP_STATUS_DATA);
            }

            edge_count = __atomic_load_n(&p_sensor->edge_count, __ATOMIC_ACQUIRE);
            if (0U < edge_count)
            {
                p_sensor->state = SENSOR_DHT11_STATE_CAPTURE;
                return APP_STATUS_BUSY;
            }
            if (now_us >= p_sensor->deadline_us)
            {
                sensor_dht11_increment(&p_sensor->diagnostics.no_responses);
                return sensor_dht11_enter_error(p_sensor,
                                                SENSOR_DHT11_FAULT_NO_RESPONSE,
                                                APP_STATUS_TIMEOUT);
            }
            return APP_STATUS_BUSY;
        }

        case SENSOR_DHT11_STATE_CAPTURE:
        {
            overflow_events = __atomic_load_n(&p_sensor->edge_overflow_events,
                                               __ATOMIC_ACQUIRE);
            edge_count = __atomic_load_n(&p_sensor->edge_count, __ATOMIC_ACQUIRE);
            if ((0U < overflow_events) ||
                (edge_count > SENSOR_DHT11_EDGE_CAPACITY))
            {
                sensor_dht11_increment(&p_sensor->diagnostics.edge_count_errors);
                sensor_dht11_add(&p_sensor->diagnostics.edge_overflows,
                                 overflow_events);
                return sensor_dht11_enter_error(p_sensor,
                                                SENSOR_DHT11_FAULT_EDGE_COUNT,
                                                APP_STATUS_DATA);
            }
            if (edge_count >= SENSOR_DHT11_REQUIRED_EDGE_COUNT)
            {
                status = sensor_dht11_finish_capture(p_sensor);
                if (APP_STATUS_OK != status)
                {
                    sensor_dht11_increment(&p_sensor->diagnostics.hardware_errors);
                    return sensor_dht11_enter_error(p_sensor,
                                                    SENSOR_DHT11_FAULT_HARDWARE,
                                                    status);
                }
                p_sensor->diagnostics.last_edge_count = edge_count;
                if (edge_count > p_sensor->diagnostics.max_edge_count)
                {
                    p_sensor->diagnostics.max_edge_count = edge_count;
                }
                p_sensor->state = SENSOR_DHT11_STATE_DECODE;
                return APP_STATUS_BUSY;
            }
            if (now_us >= p_sensor->capture_deadline_us)
            {
                sensor_dht11_increment(&p_sensor->diagnostics.capture_timeouts);
                sensor_dht11_increment(&p_sensor->diagnostics.edge_count_errors);
                p_sensor->diagnostics.last_edge_count = edge_count;
                return sensor_dht11_enter_error(p_sensor,
                                                SENSOR_DHT11_FAULT_CAPTURE_TIMEOUT,
                                                APP_STATUS_TIMEOUT);
            }
            return APP_STATUS_BUSY;
        }

        case SENSOR_DHT11_STATE_DECODE:
        {
            status = sensor_dht11_decode(p_sensor);
            if (APP_STATUS_OK != status)
            {
                sensor_dht11_increment(&p_sensor->diagnostics.edge_count_errors);
                return sensor_dht11_enter_error(p_sensor,
                                                SENSOR_DHT11_FAULT_EDGE_COUNT,
                                                status);
            }
            p_sensor->state = SENSOR_DHT11_STATE_VALIDATE;
            return APP_STATUS_BUSY;
        }

        case SENSOR_DHT11_STATE_VALIDATE:
        {
            return sensor_dht11_validate(p_sensor, now_us);
        }

        case SENSOR_DHT11_STATE_ERROR:
        {
            status = sensor_dht11_finish_capture(p_sensor);
            if (APP_STATUS_OK != status)
            {
                sensor_dht11_increment(&p_sensor->diagnostics.hardware_errors);
                p_sensor->diagnostics.last_status = status;
                p_sensor->diagnostics.last_fault = SENSOR_DHT11_FAULT_HARDWARE;
            }
            p_sensor->deadline_us = now_us + p_sensor->config.backoff_us;
            if (p_sensor->deadline_us < p_sensor->next_request_us)
            {
                p_sensor->deadline_us = p_sensor->next_request_us;
            }
            p_sensor->state = SENSOR_DHT11_STATE_BACKOFF;
            return p_sensor->diagnostics.last_status;
        }

        case SENSOR_DHT11_STATE_BACKOFF:
        {
            if (now_us < p_sensor->deadline_us)
            {
                return APP_STATUS_BUSY;
            }
            p_sensor->state = SENSOR_DHT11_STATE_IDLE;
            return APP_STATUS_OK;
        }

        default:
        {
            return sensor_dht11_enter_error(p_sensor,
                                            SENSOR_DHT11_FAULT_HARDWARE,
                                            APP_STATUS_DATA);
        }
    }
}

void sensor_dht11_on_edge_isr (sensor_dht11_t * p_sensor,
                               uint32_t raw_cycles,
                               bool level_high)
{
    uint32_t index;

    if ((NULL == p_sensor) ||
        !__atomic_load_n(&p_sensor->capture_enabled, __ATOMIC_ACQUIRE))
    {
        return;
    }

    index = __atomic_load_n(&p_sensor->edge_count, __ATOMIC_RELAXED);
    if (index >= SENSOR_DHT11_EDGE_CAPACITY)
    {
        sensor_dht11_increment_from_isr(&p_sensor->edge_overflow_events);
        return;
    }

    p_sensor->edges[index].raw_cycles = raw_cycles;
    p_sensor->edges[index].level_high = level_high;
    __atomic_store_n(&p_sensor->edge_count, index + 1U, __ATOMIC_RELEASE);
}

app_status_t sensor_dht11_get_reading (sensor_dht11_t * p_sensor,
                                       uint64_t now_us,
                                       sensor_dht11_reading_t * p_reading)
{
    if ((NULL == p_sensor) || (NULL == p_reading))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (!p_sensor->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    sensor_dht11_update_stale(p_sensor, now_us);
    *p_reading = p_sensor->reading;
    p_reading->age_ms = p_reading->has_value ?
                        sensor_dht11_age_ms(now_us, p_reading->sample_time_us) :
                        UINT32_MAX;

    return p_reading->has_value ? APP_STATUS_OK : APP_STATUS_UNAVAILABLE;
}

app_status_t sensor_dht11_get_status (sensor_dht11_t const * p_sensor,
                                      sensor_dht11_state_t * p_state,
                                      sensor_dht11_fault_t * p_fault)
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

void sensor_dht11_get_diagnostics (sensor_dht11_t const * p_sensor,
                                   sensor_dht11_diagnostics_t * p_diagnostics)
{
    if ((NULL != p_sensor) && (NULL != p_diagnostics))
    {
        *p_diagnostics = p_sensor->diagnostics;
    }
}

app_status_t sensor_dht11_close (sensor_dht11_t * p_sensor)
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

    status = sensor_dht11_finish_capture(p_sensor);
    p_sensor->state = SENSOR_DHT11_STATE_IDLE;
    p_sensor->initialized = false;

    return status;
}

char const * sensor_dht11_state_name (sensor_dht11_state_t state)
{
    static char const * const names[] =
    {
        "idle", "start-low", "release", "capture", "decode",
        "validate", "ready", "error", "backoff"
    };

    if ((uint32_t) state >= (uint32_t) (sizeof(names) / sizeof(names[0])))
    {
        return "unknown";
    }

    return names[state];
}

char const * sensor_dht11_fault_name (sensor_dht11_fault_t fault)
{
    static char const * const names[] =
    {
        "none", "no-response", "capture-timeout", "edge-count",
        "checksum", "range", "hardware"
    };

    if ((uint32_t) fault >= (uint32_t) (sizeof(names) / sizeof(names[0])))
    {
        return "unknown";
    }

    return names[fault];
}

static void sensor_dht11_increment (uint32_t * p_value)
{
    if ((NULL != p_value) && (UINT32_MAX != *p_value))
    {
        (*p_value)++;
    }
}

static void sensor_dht11_add (uint32_t * p_value, uint32_t amount)
{
    if (NULL != p_value)
    {
        *p_value = (amount > (UINT32_MAX - *p_value)) ?
                   UINT32_MAX : (*p_value + amount);
    }
}

static void sensor_dht11_increment_from_isr (volatile uint32_t * p_value)
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

static bool sensor_dht11_config_valid (sensor_dht11_config_t const * p_config)
{
    return (NULL != p_config) &&
           (p_config->start_low_us >= SENSOR_DHT11_DEFAULT_START_LOW_US) &&
           (0U < p_config->response_timeout_us) &&
           (0U < p_config->capture_timeout_us) &&
           (0U < p_config->backoff_us) &&
           (p_config->pulse_min_us < p_config->one_threshold_us) &&
           (p_config->one_threshold_us < p_config->pulse_max_us) &&
           (p_config->minimum_sample_interval_us >= SENSOR_DHT11_MIN_SAMPLE_INTERVAL_US) &&
           (0U < p_config->stale_after_us) &&
           (p_config->temperature_min_milli_c < p_config->temperature_max_milli_c) &&
           (p_config->humidity_min_milli_percent < p_config->humidity_max_milli_percent);
}

static bool sensor_dht11_ops_valid (sensor_dht11_ops_t const * p_ops)
{
    return (NULL != p_ops) &&
           (NULL != p_ops->drive_low) &&
           (NULL != p_ops->release_and_arm_capture) &&
           (NULL != p_ops->disarm_capture) &&
           (NULL != p_ops->elapsed_us);
}

static app_status_t sensor_dht11_finish_capture (sensor_dht11_t * p_sensor)
{
    app_status_t status = APP_STATUS_OK;

    __atomic_store_n(&p_sensor->capture_enabled, false, __ATOMIC_RELEASE);
    if (p_sensor->capture_armed)
    {
        status = p_sensor->ops.disarm_capture(p_sensor->p_context);
        p_sensor->capture_armed = false;
    }

    return status;
}

static app_status_t sensor_dht11_enter_error (sensor_dht11_t * p_sensor,
                                              sensor_dht11_fault_t fault,
                                              app_status_t status)
{
    p_sensor->reading.current_sample_valid = false;
    p_sensor->diagnostics.last_fault = fault;
    p_sensor->diagnostics.last_status = status;
    p_sensor->state = SENSOR_DHT11_STATE_ERROR;

    return status;
}

static app_status_t sensor_dht11_decode (sensor_dht11_t * p_sensor)
{
    uint32_t bit_index;

    if (__atomic_load_n(&p_sensor->edge_count, __ATOMIC_ACQUIRE) <
        SENSOR_DHT11_REQUIRED_EDGE_COUNT)
    {
        return APP_STATUS_DATA;
    }
    if (p_sensor->edges[0].level_high || !p_sensor->edges[1].level_high ||
        p_sensor->edges[2].level_high)
    {
        return APP_STATUS_DATA;
    }

    (void) memset(p_sensor->decoded, 0, sizeof(p_sensor->decoded));
    for (bit_index = 0U; bit_index < 40U; bit_index++)
    {
        uint32_t const rise_index = 3U + (bit_index * 2U);
        uint32_t const fall_index = rise_index + 1U;
        uint32_t pulse_us = 0U;
        uint32_t const byte_index = bit_index / 8U;

        if (!p_sensor->edges[rise_index].level_high ||
            p_sensor->edges[fall_index].level_high)
        {
            return APP_STATUS_DATA;
        }
        if (APP_STATUS_OK != p_sensor->ops.elapsed_us(
                p_sensor->p_context,
                p_sensor->edges[rise_index].raw_cycles,
                p_sensor->edges[fall_index].raw_cycles,
                &pulse_us))
        {
            return APP_STATUS_DATA;
        }
        if ((pulse_us < p_sensor->config.pulse_min_us) ||
            (pulse_us > p_sensor->config.pulse_max_us))
        {
            return APP_STATUS_DATA;
        }

        p_sensor->decoded[byte_index] =
            (uint8_t) (p_sensor->decoded[byte_index] << 1U);
        if (pulse_us >= p_sensor->config.one_threshold_us)
        {
            p_sensor->decoded[byte_index] |= 1U;
        }
    }

    return APP_STATUS_OK;
}

static app_status_t sensor_dht11_validate (sensor_dht11_t * p_sensor,
                                           uint64_t now_us)
{
    uint8_t const checksum = (uint8_t) (p_sensor->decoded[0] +
                                        p_sensor->decoded[1] +
                                        p_sensor->decoded[2] +
                                        p_sensor->decoded[3]);
    uint32_t const humidity = ((uint32_t) p_sensor->decoded[0] * 1000U) +
                              ((uint32_t) p_sensor->decoded[1] * 100U);
    int32_t temperature = ((int32_t) (p_sensor->decoded[2] & 0x7FU) * 1000) +
                          ((int32_t) p_sensor->decoded[3] * 100);

    if (checksum != p_sensor->decoded[4])
    {
        sensor_dht11_increment(&p_sensor->diagnostics.checksum_errors);
        return sensor_dht11_enter_error(p_sensor,
                                        SENSOR_DHT11_FAULT_CHECKSUM,
                                        APP_STATUS_DATA);
    }
    if (0U != (p_sensor->decoded[2] & 0x80U))
    {
        temperature = -temperature;
    }
    if ((temperature < p_sensor->config.temperature_min_milli_c) ||
        (temperature > p_sensor->config.temperature_max_milli_c) ||
        (humidity < p_sensor->config.humidity_min_milli_percent) ||
        (humidity > p_sensor->config.humidity_max_milli_percent))
    {
        sensor_dht11_increment(&p_sensor->diagnostics.range_errors);
        return sensor_dht11_enter_error(p_sensor,
                                        SENSOR_DHT11_FAULT_RANGE,
                                        APP_STATUS_DATA);
    }

    p_sensor->reading.temperature_milli_c = temperature;
    p_sensor->reading.humidity_milli_percent = humidity;
    p_sensor->reading.sample_time_us = now_us;
    p_sensor->reading.age_ms = 0U;
    (void) memcpy(p_sensor->reading.raw, p_sensor->decoded,
                  sizeof(p_sensor->reading.raw));
    p_sensor->reading.has_value = true;
    p_sensor->reading.current_sample_valid = true;
    p_sensor->reading.stale = false;
    p_sensor->stale_reported = false;
    sensor_dht11_increment(&p_sensor->diagnostics.successful_samples);
    p_sensor->diagnostics.last_fault = SENSOR_DHT11_FAULT_NONE;
    p_sensor->diagnostics.last_status = APP_STATUS_OK;
    p_sensor->state = SENSOR_DHT11_STATE_READY;

    return APP_STATUS_OK;
}

static void sensor_dht11_update_stale (sensor_dht11_t * p_sensor,
                                       uint64_t now_us)
{
    bool const stale = p_sensor->reading.has_value &&
                       (now_us >= p_sensor->reading.sample_time_us) &&
                       ((now_us - p_sensor->reading.sample_time_us) >=
                        p_sensor->config.stale_after_us);

    p_sensor->reading.stale = stale;
    if (stale && !p_sensor->stale_reported)
    {
        sensor_dht11_increment(&p_sensor->diagnostics.stale_events);
        p_sensor->stale_reported = true;
    }
}

static uint32_t sensor_dht11_age_ms (uint64_t now_us, uint64_t sample_time_us)
{
    uint64_t age_ms;

    if (now_us < sample_time_us)
    {
        return 0U;
    }

    age_ms = (now_us - sample_time_us) / 1000U;
    return (age_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t) age_ms;
}

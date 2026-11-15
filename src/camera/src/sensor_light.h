#ifndef SENSOR_LIGHT_H_
#define SENSOR_LIGHT_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define SENSOR_LIGHT_SAMPLES_PER_CYCLE      (8U)
#define SENSOR_LIGHT_MIN_CYCLE_INTERVAL_US  (100000ULL)
#define SENSOR_LIGHT_PERCENT_TENTHS_MAX     (1000U)

typedef enum e_sensor_light_state
{
    SENSOR_LIGHT_STATE_IDLE = 0,
    SENSOR_LIGHT_STATE_SAMPLING,
    SENSOR_LIGHT_STATE_READY,
    SENSOR_LIGHT_STATE_ERROR,
    SENSOR_LIGHT_STATE_BACKOFF
} sensor_light_state_t;

typedef enum e_sensor_light_fault
{
    SENSOR_LIGHT_FAULT_NONE = 0,
    SENSOR_LIGHT_FAULT_TIMEOUT,
    SENSOR_LIGHT_FAULT_ADC,
    SENSOR_LIGHT_FAULT_RANGE,
    SENSOR_LIGHT_FAULT_EVENT_OVERFLOW,
    SENSOR_LIGHT_FAULT_HARDWARE
} sensor_light_fault_t;

typedef struct st_sensor_light_config
{
    uint16_t adc_max_raw;
    uint16_t calibration_dark_raw;
    uint16_t calibration_bright_raw;
    uint16_t calibration_version;
    uint32_t sample_timeout_us;
    uint32_t backoff_us;
    uint64_t minimum_cycle_interval_us;
    uint64_t stale_after_us;
    bool     reverse_percent;
} sensor_light_config_t;

typedef struct st_sensor_light_ops
{
    app_status_t (* start_sample)(void * p_context);
    app_status_t (* read_sample)(void * p_context, uint16_t * p_raw);
    app_status_t (* stop)(void * p_context);
} sensor_light_ops_t;

typedef struct st_sensor_light_reading
{
    uint16_t raw;
    uint16_t filtered_average;
    uint16_t calibrated_tenths_percent;
    uint16_t calibration_version;
    uint64_t sample_time_us;
    uint32_t age_ms;
    bool     has_value;
    bool     current_cycle_valid;
    bool     stale;
} sensor_light_reading_t;

typedef struct st_sensor_light_diagnostics
{
    uint32_t cycles_requested;
    uint32_t cycles_completed;
    uint32_t sample_requests;
    uint32_t samples_completed;
    uint32_t rate_limited_requests;
    uint32_t timeouts;
    uint32_t adc_errors;
    uint32_t range_errors;
    uint32_t event_overflows;
    uint32_t hardware_errors;
    uint32_t stale_events;
    uint32_t last_cycle_samples;
    uint32_t max_cycle_samples;
    app_status_t last_status;
    sensor_light_fault_t last_fault;
} sensor_light_diagnostics_t;

typedef struct st_sensor_light
{
    sensor_light_config_t config;
    sensor_light_ops_t    ops;
    void *                p_context;
    sensor_light_state_t  state;
    sensor_light_reading_t reading;
    sensor_light_diagnostics_t diagnostics;
    uint16_t samples[SENSOR_LIGHT_SAMPLES_PER_CYCLE];
    uint8_t sample_count;
    volatile uint32_t pending_events;
    volatile uint32_t event_overflow_events;
    volatile int32_t  pending_status;
    uint64_t deadline_us;
    uint64_t next_request_us;
    bool initialized;
    volatile bool sample_in_flight;
    bool stale_reported;
} sensor_light_t;

void         sensor_light_config_default(sensor_light_config_t * p_config);
app_status_t sensor_light_init(sensor_light_t * p_sensor,
                               sensor_light_config_t const * p_config,
                               sensor_light_ops_t const * p_ops,
                               void * p_context);
app_status_t sensor_light_request(sensor_light_t * p_sensor, uint64_t now_us);
app_status_t sensor_light_process(sensor_light_t * p_sensor, uint64_t now_us);
void         sensor_light_on_adc_event_isr(sensor_light_t * p_sensor,
                                           app_status_t event_status);
app_status_t sensor_light_get_reading(sensor_light_t * p_sensor,
                                      uint64_t now_us,
                                      sensor_light_reading_t * p_reading);
app_status_t sensor_light_get_status(sensor_light_t const * p_sensor,
                                     sensor_light_state_t * p_state,
                                     sensor_light_fault_t * p_fault);
void         sensor_light_get_diagnostics(sensor_light_t const * p_sensor,
                                          sensor_light_diagnostics_t * p_diagnostics);
app_status_t sensor_light_close(sensor_light_t * p_sensor);
uint16_t     sensor_light_calibrate_tenths_percent(sensor_light_config_t const * p_config,
                                                   uint16_t raw);
char const * sensor_light_state_name(sensor_light_state_t state);
char const * sensor_light_fault_name(sensor_light_fault_t fault);

#endif /* SENSOR_LIGHT_H_ */

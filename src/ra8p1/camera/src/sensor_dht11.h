#ifndef SENSOR_DHT11_H_
#define SENSOR_DHT11_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define SENSOR_DHT11_EDGE_CAPACITY          (84U)
#define SENSOR_DHT11_REQUIRED_EDGE_COUNT    (83U)
#define SENSOR_DHT11_RAW_BYTES              (5U)
#define SENSOR_DHT11_MIN_SAMPLE_INTERVAL_US (1000000ULL)

typedef enum e_sensor_dht11_state
{
    SENSOR_DHT11_STATE_IDLE = 0,
    SENSOR_DHT11_STATE_START_LOW,
    SENSOR_DHT11_STATE_RELEASE,
    SENSOR_DHT11_STATE_CAPTURE,
    SENSOR_DHT11_STATE_DECODE,
    SENSOR_DHT11_STATE_VALIDATE,
    SENSOR_DHT11_STATE_READY,
    SENSOR_DHT11_STATE_ERROR,
    SENSOR_DHT11_STATE_BACKOFF
} sensor_dht11_state_t;

typedef enum e_sensor_dht11_fault
{
    SENSOR_DHT11_FAULT_NONE = 0,
    SENSOR_DHT11_FAULT_NO_RESPONSE,
    SENSOR_DHT11_FAULT_CAPTURE_TIMEOUT,
    SENSOR_DHT11_FAULT_EDGE_COUNT,
    SENSOR_DHT11_FAULT_CHECKSUM,
    SENSOR_DHT11_FAULT_RANGE,
    SENSOR_DHT11_FAULT_HARDWARE
} sensor_dht11_fault_t;

typedef struct st_sensor_dht11_edge
{
    uint32_t raw_cycles;
    bool     level_high;
} sensor_dht11_edge_t;

typedef struct st_sensor_dht11_config
{
    uint32_t start_low_us;
    uint32_t response_timeout_us;
    uint32_t capture_timeout_us;
    uint32_t backoff_us;
    uint32_t one_threshold_us;
    uint32_t pulse_min_us;
    uint32_t pulse_max_us;
    uint64_t minimum_sample_interval_us;
    uint64_t stale_after_us;
    int32_t  temperature_min_milli_c;
    int32_t  temperature_max_milli_c;
    uint32_t humidity_min_milli_percent;
    uint32_t humidity_max_milli_percent;
} sensor_dht11_config_t;

typedef struct st_sensor_dht11_ops
{
    app_status_t (* drive_low)(void * p_context);
    app_status_t (* release_and_arm_capture)(void * p_context);
    app_status_t (* disarm_capture)(void * p_context);
    app_status_t (* elapsed_us)(void * p_context,
                                uint32_t start_raw_cycles,
                                uint32_t end_raw_cycles,
                                uint32_t * p_elapsed_us);
} sensor_dht11_ops_t;

typedef struct st_sensor_dht11_reading
{
    int32_t  temperature_milli_c;
    uint32_t humidity_milli_percent;
    uint64_t sample_time_us;
    uint32_t age_ms;
    uint8_t  raw[SENSOR_DHT11_RAW_BYTES];
    bool     has_value;
    bool     current_sample_valid;
    bool     stale;
} sensor_dht11_reading_t;

typedef struct st_sensor_dht11_diagnostics
{
    uint32_t requests;
    uint32_t successful_samples;
    uint32_t rate_limited_requests;
    uint32_t no_responses;
    uint32_t capture_timeouts;
    uint32_t edge_count_errors;
    uint32_t edge_overflows;
    uint32_t checksum_errors;
    uint32_t range_errors;
    uint32_t hardware_errors;
    uint32_t stale_events;
    uint32_t last_edge_count;
    uint32_t max_edge_count;
    app_status_t last_status;
    sensor_dht11_fault_t last_fault;
} sensor_dht11_diagnostics_t;

typedef struct st_sensor_dht11
{
    sensor_dht11_config_t config;
    sensor_dht11_ops_t    ops;
    void *                p_context;
    sensor_dht11_state_t  state;
    sensor_dht11_reading_t reading;
    sensor_dht11_diagnostics_t diagnostics;
    sensor_dht11_edge_t edges[SENSOR_DHT11_EDGE_CAPACITY];
    uint8_t decoded[SENSOR_DHT11_RAW_BYTES];
    volatile uint32_t edge_count;
    volatile uint32_t edge_overflow_events;
    volatile bool     capture_enabled;
    uint64_t deadline_us;
    uint64_t capture_deadline_us;
    uint64_t next_request_us;
    bool initialized;
    bool capture_armed;
    bool stale_reported;
} sensor_dht11_t;

void         sensor_dht11_config_default(sensor_dht11_config_t * p_config);
app_status_t sensor_dht11_init(sensor_dht11_t * p_sensor,
                               sensor_dht11_config_t const * p_config,
                               sensor_dht11_ops_t const * p_ops,
                               void * p_context);
app_status_t sensor_dht11_request(sensor_dht11_t * p_sensor, uint64_t now_us);
app_status_t sensor_dht11_process(sensor_dht11_t * p_sensor, uint64_t now_us);
void         sensor_dht11_on_edge_isr(sensor_dht11_t * p_sensor,
                                      uint32_t raw_cycles,
                                      bool level_high);
app_status_t sensor_dht11_get_reading(sensor_dht11_t * p_sensor,
                                      uint64_t now_us,
                                      sensor_dht11_reading_t * p_reading);
app_status_t sensor_dht11_get_status(sensor_dht11_t const * p_sensor,
                                     sensor_dht11_state_t * p_state,
                                     sensor_dht11_fault_t * p_fault);
void         sensor_dht11_get_diagnostics(sensor_dht11_t const * p_sensor,
                                          sensor_dht11_diagnostics_t * p_diagnostics);
app_status_t sensor_dht11_close(sensor_dht11_t * p_sensor);
char const * sensor_dht11_state_name(sensor_dht11_state_t state);
char const * sensor_dht11_fault_name(sensor_dht11_fault_t fault);

#endif /* SENSOR_DHT11_H_ */

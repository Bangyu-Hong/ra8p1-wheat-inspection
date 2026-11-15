#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sensor_dht11.h"
#include "sensor_light.h"

#define TEST_ASSERT(condition)                                                        \
    do                                                                                \
    {                                                                                 \
        if (!(condition))                                                             \
        {                                                                             \
            (void) fprintf(stderr, "assertion failed: %s (%s:%d)\n",                \
                           #condition, __FILE__, __LINE__);                            \
            return 1;                                                                 \
        }                                                                             \
    } while (false)

typedef struct st_fake_dht
{
    uint32_t drive_low_calls;
    uint32_t release_calls;
    uint32_t disarm_calls;
    app_status_t drive_low_status;
    app_status_t release_status;
    app_status_t disarm_status;
} fake_dht_t;

typedef struct st_fake_light
{
    uint32_t start_calls;
    uint32_t read_calls;
    uint32_t stop_calls;
    uint16_t next_raw;
    app_status_t start_status;
    app_status_t read_status;
    app_status_t stop_status;
} fake_light_t;

static app_status_t fake_dht_drive_low(void * p_context);
static app_status_t fake_dht_release(void * p_context);
static app_status_t fake_dht_disarm(void * p_context);
static app_status_t fake_dht_elapsed(void * p_context,
                                     uint32_t start_raw_cycles,
                                     uint32_t end_raw_cycles,
                                     uint32_t * p_elapsed_us);
static app_status_t fake_light_start(void * p_context);
static app_status_t fake_light_read(void * p_context, uint16_t * p_raw);
static app_status_t fake_light_stop(void * p_context);
static void dht_emit_frame(sensor_dht11_t * p_sensor,
                           uint8_t const raw[SENSOR_DHT11_RAW_BYTES]);
static int dht_begin_capture(sensor_dht11_t * p_sensor,
                             fake_dht_t * p_context,
                             uint64_t request_us);
static int dht_finish_frame(sensor_dht11_t * p_sensor, uint64_t now_us);
static int test_dht_success_rate_limit_and_stale(void);
static int test_dht_fault_classification(void);
static int test_dht_edge_overflow_and_hardware_error(void);
static int light_run_cycle(sensor_light_t * p_sensor,
                           fake_light_t * p_context,
                           uint64_t start_us,
                           uint16_t const samples[SENSOR_LIGHT_SAMPLES_PER_CYCLE]);
static int test_light_filter_calibration_and_stale(void);
static int test_light_faults_preserve_last_value(void);

int main (void)
{
    TEST_ASSERT(1000000ULL == SENSOR_DHT11_MIN_SAMPLE_INTERVAL_US);
    TEST_ASSERT(100000ULL == SENSOR_LIGHT_MIN_CYCLE_INTERVAL_US);
    TEST_ASSERT(0 == test_dht_success_rate_limit_and_stale());
    TEST_ASSERT(0 == test_dht_fault_classification());
    TEST_ASSERT(0 == test_dht_edge_overflow_and_hardware_error());
    TEST_ASSERT(0 == test_light_filter_calibration_and_stale());
    TEST_ASSERT(0 == test_light_faults_preserve_last_value());

    (void) printf("environment-sensor-tests status=ok dht=async-84-edges "
                  "light=8-sample-trimmed calibration=versioned faults=bounded\n");
    return 0;
}

static app_status_t fake_dht_drive_low (void * p_context)
{
    fake_dht_t * p_fake = (fake_dht_t *) p_context;

    p_fake->drive_low_calls++;
    return p_fake->drive_low_status;
}

static app_status_t fake_dht_release (void * p_context)
{
    fake_dht_t * p_fake = (fake_dht_t *) p_context;

    p_fake->release_calls++;
    return p_fake->release_status;
}

static app_status_t fake_dht_disarm (void * p_context)
{
    fake_dht_t * p_fake = (fake_dht_t *) p_context;

    p_fake->disarm_calls++;
    return p_fake->disarm_status;
}

static app_status_t fake_dht_elapsed (void * p_context,
                                      uint32_t start_raw_cycles,
                                      uint32_t end_raw_cycles,
                                      uint32_t * p_elapsed_us)
{
    (void) p_context;
    if (NULL == p_elapsed_us)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_elapsed_us = end_raw_cycles - start_raw_cycles;
    return APP_STATUS_OK;
}

static app_status_t fake_light_start (void * p_context)
{
    fake_light_t * p_fake = (fake_light_t *) p_context;

    p_fake->start_calls++;
    return p_fake->start_status;
}

static app_status_t fake_light_read (void * p_context, uint16_t * p_raw)
{
    fake_light_t * p_fake = (fake_light_t *) p_context;

    p_fake->read_calls++;
    if ((APP_STATUS_OK == p_fake->read_status) && (NULL != p_raw))
    {
        *p_raw = p_fake->next_raw;
    }
    return p_fake->read_status;
}

static app_status_t fake_light_stop (void * p_context)
{
    fake_light_t * p_fake = (fake_light_t *) p_context;

    p_fake->stop_calls++;
    return p_fake->stop_status;
}

static void dht_emit_frame (sensor_dht11_t * p_sensor,
                            uint8_t const raw[SENSOR_DHT11_RAW_BYTES])
{
    uint32_t timestamp = UINT32_MAX - 200U;
    uint32_t bit_index;

    sensor_dht11_on_edge_isr(p_sensor, timestamp, false);
    timestamp += 80U;
    sensor_dht11_on_edge_isr(p_sensor, timestamp, true);
    timestamp += 80U;
    sensor_dht11_on_edge_isr(p_sensor, timestamp, false);

    for (bit_index = 0U; bit_index < 40U; bit_index++)
    {
        uint32_t const byte_index = bit_index / 8U;
        uint32_t const bit_in_byte = 7U - (bit_index % 8U);
        bool const one = 0U != (raw[byte_index] & (uint8_t) (1U << bit_in_byte));

        timestamp += 50U;
        sensor_dht11_on_edge_isr(p_sensor, timestamp, true);
        timestamp += one ? 70U : 28U;
        sensor_dht11_on_edge_isr(p_sensor, timestamp, false);
    }

    timestamp += 50U;
    sensor_dht11_on_edge_isr(p_sensor, timestamp, true);
}

static int dht_begin_capture (sensor_dht11_t * p_sensor,
                              fake_dht_t * p_context,
                              uint64_t request_us)
{
    sensor_dht11_config_t config;
    sensor_dht11_ops_t const ops =
    {
        .drive_low = fake_dht_drive_low,
        .release_and_arm_capture = fake_dht_release,
        .disarm_capture = fake_dht_disarm,
        .elapsed_us = fake_dht_elapsed,
    };

    sensor_dht11_config_default(&config);
    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_init(p_sensor, &config, &ops,
                                                    p_context));
    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_request(p_sensor, request_us));
    TEST_ASSERT(SENSOR_DHT11_STATE_START_LOW == p_sensor->state);
    TEST_ASSERT(APP_STATUS_BUSY == sensor_dht11_process(
        p_sensor, request_us + config.start_low_us - 1U));
    TEST_ASSERT(APP_STATUS_BUSY == sensor_dht11_process(
        p_sensor, request_us + config.start_low_us));
    TEST_ASSERT(SENSOR_DHT11_STATE_RELEASE == p_sensor->state);

    return 0;
}

static int dht_finish_frame (sensor_dht11_t * p_sensor, uint64_t now_us)
{
    TEST_ASSERT(APP_STATUS_BUSY == sensor_dht11_process(p_sensor, now_us));
    TEST_ASSERT(SENSOR_DHT11_STATE_CAPTURE == p_sensor->state);
    TEST_ASSERT(APP_STATUS_BUSY == sensor_dht11_process(p_sensor, now_us + 1U));
    TEST_ASSERT(SENSOR_DHT11_STATE_DECODE == p_sensor->state);
    TEST_ASSERT(APP_STATUS_BUSY == sensor_dht11_process(p_sensor, now_us + 2U));
    TEST_ASSERT(SENSOR_DHT11_STATE_VALIDATE == p_sensor->state);

    return 0;
}

static int test_dht_success_rate_limit_and_stale (void)
{
    sensor_dht11_t sensor;
    sensor_dht11_reading_t reading;
    sensor_dht11_diagnostics_t diagnostics;
    fake_dht_t fake = {0};
    uint8_t const raw[SENSOR_DHT11_RAW_BYTES] = {55U, 0U, 24U, 0U, 79U};
    uint64_t const request_us = 1000U;
    uint64_t const validate_us = 19004U;

    TEST_ASSERT(0 == dht_begin_capture(&sensor, &fake, request_us));
    dht_emit_frame(&sensor, raw);
    TEST_ASSERT(0 == dht_finish_frame(&sensor, validate_us - 2U));
    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_process(&sensor, validate_us));
    TEST_ASSERT(SENSOR_DHT11_STATE_READY == sensor.state);
    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_get_reading(&sensor, validate_us,
                                                           &reading));
    TEST_ASSERT(reading.has_value);
    TEST_ASSERT(reading.current_sample_valid);
    TEST_ASSERT(!reading.stale);
    TEST_ASSERT(24000 == reading.temperature_milli_c);
    TEST_ASSERT(55000U == reading.humidity_milli_percent);
    TEST_ASSERT(1U == fake.drive_low_calls);
    TEST_ASSERT(1U == fake.release_calls);
    TEST_ASSERT(1U == fake.disarm_calls);

    TEST_ASSERT(APP_STATUS_BUSY == sensor_dht11_request(
        &sensor, request_us + SENSOR_DHT11_MIN_SAMPLE_INTERVAL_US - 1U));
    sensor_dht11_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.rate_limited_requests);
    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_request(
        &sensor, request_us + SENSOR_DHT11_MIN_SAMPLE_INTERVAL_US));
    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_close(&sensor));

    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_init(
        &sensor, &(sensor_dht11_config_t)
        {
            .start_low_us = 18000U,
            .response_timeout_us = 300U,
            .capture_timeout_us = 6000U,
            .backoff_us = 250000U,
            .one_threshold_us = 50U,
            .pulse_min_us = 15U,
            .pulse_max_us = 120U,
            .minimum_sample_interval_us = SENSOR_DHT11_MIN_SAMPLE_INTERVAL_US,
            .stale_after_us = 5000000U,
            .temperature_min_milli_c = 0,
            .temperature_max_milli_c = 50000,
            .humidity_min_milli_percent = 20000U,
            .humidity_max_milli_percent = 90000U,
        },
        &(sensor_dht11_ops_t)
        {
            .drive_low = fake_dht_drive_low,
            .release_and_arm_capture = fake_dht_release,
            .disarm_capture = fake_dht_disarm,
            .elapsed_us = fake_dht_elapsed,
        },
        &fake));
    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_request(&sensor, 0U));
    TEST_ASSERT(APP_STATUS_BUSY == sensor_dht11_process(&sensor, 18000U));
    dht_emit_frame(&sensor, raw);
    TEST_ASSERT(0 == dht_finish_frame(&sensor, 18001U));
    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_process(&sensor, 18003U));
    TEST_ASSERT(APP_STATUS_OK == sensor_dht11_get_reading(
        &sensor, 5018003U, &reading));
    TEST_ASSERT(reading.stale);
    TEST_ASSERT(5000U == reading.age_ms);
    sensor_dht11_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.successful_samples);
    TEST_ASSERT(1U == diagnostics.stale_events);
    TEST_ASSERT(0U == diagnostics.rate_limited_requests);

    return 0;
}

static int test_dht_fault_classification (void)
{
    sensor_dht11_t sensor;
    sensor_dht11_diagnostics_t diagnostics;
    fake_dht_t fake = {0};
    uint8_t const bad_checksum[SENSOR_DHT11_RAW_BYTES] = {55U, 0U, 24U, 0U, 78U};
    uint8_t const bad_range[SENSOR_DHT11_RAW_BYTES] = {10U, 0U, 24U, 0U, 34U};

    TEST_ASSERT(0 == dht_begin_capture(&sensor, &fake, 0U));
    TEST_ASSERT(APP_STATUS_TIMEOUT == sensor_dht11_process(&sensor, 18300U));
    sensor_dht11_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.no_responses);
    TEST_ASSERT(0U == diagnostics.capture_timeouts);
    TEST_ASSERT(SENSOR_DHT11_FAULT_NO_RESPONSE == diagnostics.last_fault);

    fake = (fake_dht_t) {0};
    TEST_ASSERT(0 == dht_begin_capture(&sensor, &fake, 0U));
    sensor_dht11_on_edge_isr(&sensor, 100U, false);
    TEST_ASSERT(APP_STATUS_BUSY == sensor_dht11_process(&sensor, 18001U));
    TEST_ASSERT(APP_STATUS_TIMEOUT == sensor_dht11_process(&sensor, 24001U));
    sensor_dht11_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.capture_timeouts);
    TEST_ASSERT(1U == diagnostics.edge_count_errors);
    TEST_ASSERT(SENSOR_DHT11_FAULT_CAPTURE_TIMEOUT == diagnostics.last_fault);

    fake = (fake_dht_t) {0};
    TEST_ASSERT(0 == dht_begin_capture(&sensor, &fake, 0U));
    dht_emit_frame(&sensor, bad_checksum);
    TEST_ASSERT(0 == dht_finish_frame(&sensor, 18001U));
    TEST_ASSERT(APP_STATUS_DATA == sensor_dht11_process(&sensor, 18003U));
    sensor_dht11_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.checksum_errors);
    TEST_ASSERT(SENSOR_DHT11_FAULT_CHECKSUM == diagnostics.last_fault);

    fake = (fake_dht_t) {0};
    TEST_ASSERT(0 == dht_begin_capture(&sensor, &fake, 0U));
    dht_emit_frame(&sensor, bad_range);
    TEST_ASSERT(0 == dht_finish_frame(&sensor, 18001U));
    TEST_ASSERT(APP_STATUS_DATA == sensor_dht11_process(&sensor, 18003U));
    sensor_dht11_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.range_errors);
    TEST_ASSERT(SENSOR_DHT11_FAULT_RANGE == diagnostics.last_fault);

    return 0;
}

static int test_dht_edge_overflow_and_hardware_error (void)
{
    sensor_dht11_t sensor;
    sensor_dht11_diagnostics_t diagnostics;
    fake_dht_t fake = {0};
    uint32_t index;

    TEST_ASSERT(0 == dht_begin_capture(&sensor, &fake, 0U));
    for (index = 0U; index < (SENSOR_DHT11_EDGE_CAPACITY + 2U); index++)
    {
        sensor_dht11_on_edge_isr(&sensor, index, 0U != (index & 1U));
    }
    TEST_ASSERT(APP_STATUS_DATA == sensor_dht11_process(&sensor, 18001U));
    sensor_dht11_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.edge_count_errors);
    TEST_ASSERT(2U == diagnostics.edge_overflows);
    TEST_ASSERT(SENSOR_DHT11_FAULT_EDGE_COUNT == diagnostics.last_fault);

    fake = (fake_dht_t) {.drive_low_status = APP_STATUS_IO};
    {
        sensor_dht11_config_t config;
        sensor_dht11_ops_t const ops =
        {
            .drive_low = fake_dht_drive_low,
            .release_and_arm_capture = fake_dht_release,
            .disarm_capture = fake_dht_disarm,
            .elapsed_us = fake_dht_elapsed,
        };

        sensor_dht11_config_default(&config);
        TEST_ASSERT(APP_STATUS_OK == sensor_dht11_init(&sensor, &config, &ops,
                                                        &fake));
        TEST_ASSERT(APP_STATUS_IO == sensor_dht11_request(&sensor, 0U));
        sensor_dht11_get_diagnostics(&sensor, &diagnostics);
        TEST_ASSERT(1U == diagnostics.hardware_errors);
        TEST_ASSERT(SENSOR_DHT11_STATE_ERROR == sensor.state);
    }

    TEST_ASSERT(0 == strcmp(sensor_dht11_state_name(SENSOR_DHT11_STATE_CAPTURE),
                            "capture"));
    TEST_ASSERT(0 == strcmp(sensor_dht11_fault_name(SENSOR_DHT11_FAULT_CHECKSUM),
                            "checksum"));

    return 0;
}

static int light_run_cycle (sensor_light_t * p_sensor,
                            fake_light_t * p_context,
                            uint64_t start_us,
                            uint16_t const samples[SENSOR_LIGHT_SAMPLES_PER_CYCLE])
{
    uint32_t index;

    TEST_ASSERT(APP_STATUS_OK == sensor_light_request(p_sensor, start_us));
    TEST_ASSERT(APP_STATUS_BUSY == sensor_light_process(p_sensor, start_us));
    for (index = 0U; index < SENSOR_LIGHT_SAMPLES_PER_CYCLE; index++)
    {
        p_context->next_raw = samples[index];
        sensor_light_on_adc_event_isr(p_sensor, APP_STATUS_OK);
        if ((index + 1U) < SENSOR_LIGHT_SAMPLES_PER_CYCLE)
        {
            TEST_ASSERT(APP_STATUS_BUSY == sensor_light_process(
                p_sensor, start_us + index + 1U));
        }
        else
        {
            TEST_ASSERT(APP_STATUS_OK == sensor_light_process(
                p_sensor, start_us + index + 1U));
        }
    }

    return 0;
}

static int test_light_filter_calibration_and_stale (void)
{
    sensor_light_t sensor;
    sensor_light_config_t config;
    sensor_light_reading_t reading;
    sensor_light_diagnostics_t diagnostics;
    sensor_light_ops_t const ops =
    {
        .start_sample = fake_light_start,
        .read_sample = fake_light_read,
        .stop = fake_light_stop,
    };
    fake_light_t fake = {0};
    uint16_t const samples[SENSOR_LIGHT_SAMPLES_PER_CYCLE] =
        {100U, 110U, 120U, 130U, 140U, 150U, 160U, 1000U};

    sensor_light_config_default(&config);
    config.calibration_dark_raw = 100U;
    config.calibration_bright_raw = 200U;
    config.calibration_version = 7U;
    TEST_ASSERT(APP_STATUS_OK == sensor_light_init(&sensor, &config, &ops, &fake));
    TEST_ASSERT(0 == light_run_cycle(&sensor, &fake, 1000U, samples));
    TEST_ASSERT(APP_STATUS_OK == sensor_light_get_reading(&sensor, 1008U, &reading));
    TEST_ASSERT(reading.has_value);
    TEST_ASSERT(reading.current_cycle_valid);
    TEST_ASSERT(1000U == reading.raw);
    TEST_ASSERT(135U == reading.filtered_average);
    TEST_ASSERT(350U == reading.calibrated_tenths_percent);
    TEST_ASSERT(7U == reading.calibration_version);
    TEST_ASSERT(8U == fake.start_calls);
    TEST_ASSERT(8U == fake.read_calls);

    TEST_ASSERT(APP_STATUS_BUSY == sensor_light_request(
        &sensor, 1000U + SENSOR_LIGHT_MIN_CYCLE_INTERVAL_US - 1U));
    TEST_ASSERT(APP_STATUS_OK == sensor_light_get_reading(
        &sensor, 1008U + config.stale_after_us, &reading));
    TEST_ASSERT(reading.stale);
    sensor_light_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.cycles_completed);
    TEST_ASSERT(8U == diagnostics.samples_completed);
    TEST_ASSERT(1U == diagnostics.rate_limited_requests);
    TEST_ASSERT(1U == diagnostics.stale_events);

    config.reverse_percent = true;
    TEST_ASSERT(650U == sensor_light_calibrate_tenths_percent(&config, 135U));
    TEST_ASSERT(1000U == sensor_light_calibrate_tenths_percent(&config, 100U));
    TEST_ASSERT(0U == sensor_light_calibrate_tenths_percent(&config, 200U));
    TEST_ASSERT(0 == strcmp(sensor_light_state_name(SENSOR_LIGHT_STATE_SAMPLING),
                            "sampling"));
    TEST_ASSERT(0 == strcmp(sensor_light_fault_name(SENSOR_LIGHT_FAULT_ADC),
                            "adc"));

    return 0;
}

static int test_light_faults_preserve_last_value (void)
{
    sensor_light_t sensor;
    sensor_light_config_t config;
    sensor_light_reading_t reading;
    sensor_light_diagnostics_t diagnostics;
    sensor_light_ops_t const ops =
    {
        .start_sample = fake_light_start,
        .read_sample = fake_light_read,
        .stop = fake_light_stop,
    };
    fake_light_t fake = {0};
    uint16_t const samples[SENSOR_LIGHT_SAMPLES_PER_CYCLE] =
        {100U, 101U, 102U, 103U, 104U, 105U, 106U, 107U};
    uint64_t const second_cycle = SENSOR_LIGHT_MIN_CYCLE_INTERVAL_US;

    sensor_light_config_default(&config);
    TEST_ASSERT(APP_STATUS_OK == sensor_light_init(&sensor, &config, &ops, &fake));
    TEST_ASSERT(0 == light_run_cycle(&sensor, &fake, 0U, samples));

    TEST_ASSERT(APP_STATUS_OK == sensor_light_request(&sensor, second_cycle));
    TEST_ASSERT(APP_STATUS_BUSY == sensor_light_process(&sensor, second_cycle));
    sensor_light_on_adc_event_isr(&sensor, APP_STATUS_IO);
    TEST_ASSERT(APP_STATUS_IO == sensor_light_process(&sensor, second_cycle + 1U));
    TEST_ASSERT(APP_STATUS_OK == sensor_light_get_reading(
        &sensor, second_cycle + 1U, &reading));
    TEST_ASSERT(reading.has_value);
    TEST_ASSERT(!reading.current_cycle_valid);
    sensor_light_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.adc_errors);
    TEST_ASSERT(SENSOR_LIGHT_FAULT_ADC == diagnostics.last_fault);

    fake = (fake_light_t) {0};
    TEST_ASSERT(APP_STATUS_OK == sensor_light_init(&sensor, &config, &ops, &fake));
    TEST_ASSERT(APP_STATUS_OK == sensor_light_request(&sensor, 0U));
    TEST_ASSERT(APP_STATUS_BUSY == sensor_light_process(&sensor, 0U));
    TEST_ASSERT(APP_STATUS_TIMEOUT == sensor_light_process(
        &sensor, config.sample_timeout_us));
    sensor_light_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.timeouts);
    TEST_ASSERT(SENSOR_LIGHT_FAULT_TIMEOUT == diagnostics.last_fault);

    fake = (fake_light_t) {0};
    TEST_ASSERT(APP_STATUS_OK == sensor_light_init(&sensor, &config, &ops, &fake));
    TEST_ASSERT(APP_STATUS_OK == sensor_light_request(&sensor, 0U));
    TEST_ASSERT(APP_STATUS_BUSY == sensor_light_process(&sensor, 0U));
    fake.next_raw = (uint16_t) (config.adc_max_raw + 1U);
    sensor_light_on_adc_event_isr(&sensor, APP_STATUS_OK);
    TEST_ASSERT(APP_STATUS_DATA == sensor_light_process(&sensor, 1U));
    sensor_light_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.range_errors);

    fake = (fake_light_t) {0};
    TEST_ASSERT(APP_STATUS_OK == sensor_light_init(&sensor, &config, &ops, &fake));
    TEST_ASSERT(APP_STATUS_OK == sensor_light_request(&sensor, 0U));
    TEST_ASSERT(APP_STATUS_BUSY == sensor_light_process(&sensor, 0U));
    sensor_light_on_adc_event_isr(&sensor, APP_STATUS_OK);
    sensor_light_on_adc_event_isr(&sensor, APP_STATUS_OK);
    TEST_ASSERT(APP_STATUS_DATA == sensor_light_process(&sensor, 1U));
    sensor_light_get_diagnostics(&sensor, &diagnostics);
    TEST_ASSERT(1U == diagnostics.event_overflows);
    TEST_ASSERT(SENSOR_LIGHT_FAULT_EVENT_OVERFLOW == diagnostics.last_fault);

    return 0;
}

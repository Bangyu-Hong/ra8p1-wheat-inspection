#ifndef ENVIRONMENT_SERVICE_H_
#define ENVIRONMENT_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "sensor_dht11.h"
#include "sensor_light.h"

typedef struct st_environment_service_hardware_diagnostics
{
    uint32_t requests;
    uint32_t request_busy;
    uint32_t process_errors;
    uint32_t dht11_pin_read_errors;
    uint32_t light_calibrations_completed;
    uint32_t light_calibration_errors;
    uint32_t light_calibration_timeouts;
    uint32_t light_callback_errors;
    uint32_t light_callback_overflows;
    uint32_t light_ignored_callbacks;
    app_status_t last_request_status;
    app_status_t last_process_status;
    app_status_t dht11_init_status;
    app_status_t light_init_status;
} environment_service_hardware_diagnostics_t;

typedef struct st_environment_service_status
{
    bool initialized;
    bool dht11_available;
    bool light_available;
    sensor_dht11_state_t dht11_state;
    sensor_dht11_fault_t dht11_fault;
    sensor_dht11_reading_t dht11_reading;
    sensor_dht11_diagnostics_t dht11_diagnostics;
    sensor_light_state_t light_state;
    sensor_light_fault_t light_fault;
    sensor_light_reading_t light_reading;
    sensor_light_diagnostics_t light_diagnostics;
    environment_service_hardware_diagnostics_t hardware;
} environment_service_status_t;

app_status_t environment_service_init(uint64_t now_us);
app_status_t environment_service_request_dht11(uint64_t now_us);
app_status_t environment_service_request_light(uint64_t now_us);
app_status_t environment_service_process(uint64_t now_us);
app_status_t environment_service_get_status(uint64_t now_us,
                                            environment_service_status_t * p_status);
app_status_t environment_service_close(void);

#endif /* ENVIRONMENT_SERVICE_H_ */

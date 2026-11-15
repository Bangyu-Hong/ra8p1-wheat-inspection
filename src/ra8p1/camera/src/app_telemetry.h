#ifndef APP_TELEMETRY_H_
#define APP_TELEMETRY_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "ra8p_wire_contract.h"

#define APP_TELEMETRY_DEVICE_ID_BYTES      (32U)
#define APP_TELEMETRY_SESSION_ID_BYTES     (40U)
#define APP_TELEMETRY_BUILD_VERSION_BYTES  (32U)
#define APP_TELEMETRY_MODEL_VERSION_BYTES  (48U)
#define APP_TELEMETRY_SCHEMA_VERSION       (1U)
#define APP_TELEMETRY_WHEAT_COUNT_UNKNOWN  (-1)

typedef enum e_app_region_status
{
    APP_REGION_STATUS_UNPLANNED = 0,
    APP_REGION_STATUS_PENDING,
    APP_REGION_STATUS_MOVING,
    APP_REGION_STATUS_CAPTURING,
    APP_REGION_STATUS_INFERENCING,
    APP_REGION_STATUS_COMPLETED,
    APP_REGION_STATUS_FAILED
} app_region_status_t;

/* Hardware services not yet connected are intentionally absent. */
#define APP_TELEMETRY_FOUNDATION_CAPABILITIES \
    (RA8P_CAPABILITY_VISION | RA8P_CAPABILITY_MISSION_PLAN | \
     RA8P_CAPABILITY_AI_INFERENCE)

typedef struct st_app_telemetry_snapshot
{
    uint16_t schema_version;
    uint32_t sequence;
    uint64_t monotonic_us;
    uint64_t wall_time_ms;
    bool     wall_time_valid;
    uint32_t capabilities;
    char     device_id[APP_TELEMETRY_DEVICE_ID_BYTES];
    char     session_id[APP_TELEMETRY_SESSION_ID_BYTES];
    char     build_version[APP_TELEMETRY_BUILD_VERSION_BYTES];
    int32_t  temperature_milli_c;
    uint32_t humidity_milli_percent;
    bool     environment_valid;
    uint16_t dht11_fault_code;
    uint32_t environment_age_ms;
    uint16_t light_raw;
    uint16_t light_average;
    uint16_t light_tenths_percent;
    uint16_t light_calibration_version;
    bool     light_valid;
    uint16_t light_fault_code;
    int32_t  wheat_count;
    uint32_t inference_us;
    char     model_version[APP_TELEMETRY_MODEL_VERSION_BYTES];
    uint16_t field_row;
    uint16_t field_column;
    uint16_t slide_position_index;
    uint8_t  region_status;
    uint32_t mission_wheat_count;
    uint32_t field_wheat_count;
    uint32_t counted_frame_count;
    uint32_t target_position_mm;
    int32_t  actual_position_mm;
    uint32_t fault_flags;
    uint8_t  camera_state;
    uint8_t  ai_state;
    uint8_t  sd_state;
    uint8_t  network_state;
    uint8_t  mission_state;
} app_telemetry_snapshot_t;

typedef struct st_app_telemetry_store
{
    app_telemetry_snapshot_t slots[2];
    uint8_t active_slot;
    bool initialized;
    uint32_t publishes;
    uint32_t reads;
} app_telemetry_store_t;

app_status_t app_telemetry_init(app_telemetry_store_t * p_store,
                                app_telemetry_snapshot_t const * p_initial);
app_status_t app_telemetry_publish(app_telemetry_store_t * p_store,
                                   app_telemetry_snapshot_t const * p_snapshot);
app_status_t app_telemetry_get(app_telemetry_store_t * p_store,
                               app_telemetry_snapshot_t * p_snapshot);

#endif /* APP_TELEMETRY_H_ */

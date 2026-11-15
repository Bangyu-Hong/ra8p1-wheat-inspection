#ifndef MISSION_PLAN_H_
#define MISSION_PLAN_H_

#include <stdint.h>

#include "app_status.h"
#include "ra8p_wire_contract.h"

#define MISSION_PLAN_SCHEMA_VERSION       (1U)
#define MISSION_PLAN_ID_BYTES             (37U)
#define MISSION_PLAN_MAX_SLIDE_POSITIONS  (32U)
#define MISSION_PLAN_SLIDE_MIN_MM          (0)
#define MISSION_PLAN_SLIDE_MAX_MM          (300)

typedef enum e_mission_route_mode
{
    MISSION_ROUTE_SERPENTINE = 0,
    MISSION_ROUTE_RASTER
} mission_route_mode_t;

typedef enum e_mission_slide_mode
{
    MISSION_SLIDE_FIXED = 0,
    MISSION_SLIDE_SCAN
} mission_slide_mode_t;

typedef enum e_mission_error_policy
{
    MISSION_ERROR_PAUSE = 0,
    MISSION_ERROR_ABORT,
    MISSION_ERROR_RETRY_ONCE
} mission_error_policy_t;

typedef enum e_mission_issue
{
    MISSION_ISSUE_NONE                = 0U,
    MISSION_ISSUE_SCHEMA_VERSION      = (1U << 0),
    MISSION_ISSUE_ROWS                = (1U << 1),
    MISSION_ISSUE_COLUMNS             = (1U << 2),
    MISSION_ISSUE_ROW_PITCH           = (1U << 3),
    MISSION_ISSUE_CELL_PITCH          = (1U << 4),
    MISSION_ISSUE_CAPTURE_INTERVAL    = (1U << 5),
    MISSION_ISSUE_TRAVEL_DISTANCE     = (1U << 6),
    MISSION_ISSUE_SLIDE_POSITIONS     = (1U << 7),
    MISSION_ISSUE_SETTLE_TIME         = (1U << 8),
    MISSION_ISSUE_SPEED               = (1U << 9),
    MISSION_ISSUE_MOVE_TIMEOUT        = (1U << 10),
    MISSION_ISSUE_INFERENCE_TIMEOUT   = (1U << 11),
    MISSION_ISSUE_ROUTE_MODE          = (1U << 12),
    MISSION_ISSUE_ERROR_POLICY        = (1U << 13),
    MISSION_ISSUE_SLIDE_MODE          = (1U << 14),
    MISSION_ISSUE_FIXED_POSITION      = (1U << 15),
    MISSION_ISSUE_CAPABILITY          = (1U << 16),
    MISSION_ISSUE_SLIDE_SCAN_DISTANCE = (1U << 17),
    MISSION_ISSUE_SLIDE_SHOT_INTERVAL = (1U << 18)
} mission_issue_t;

typedef struct st_mission_plan_v1
{
    char     plan_id[MISSION_PLAN_ID_BYTES];
    uint16_t schema_version;
    uint16_t rows;
    uint16_t columns;
    uint32_t row_pitch_mm;
    uint32_t cell_pitch_mm;
    uint32_t capture_interval_mm;
    uint32_t travel_distance_mm;
    int32_t  slide_positions_mm[MISSION_PLAN_MAX_SLIDE_POSITIONS];
    uint8_t  slide_position_count;
    mission_slide_mode_t slide_mode;
    int32_t  slide_fixed_position_mm;
    uint32_t slide_scan_distance_mm;
    uint32_t slide_shot_interval_mm;
    uint32_t settle_ms;
    uint32_t max_speed_mm_s;
    uint32_t move_timeout_ms;
    uint32_t inference_timeout_ms;
    mission_route_mode_t route_mode;
    mission_error_policy_t on_error_policy;
} mission_plan_v1_t;

typedef struct st_mission_validation
{
    app_status_t status;
    uint32_t     issue_mask;
    uint32_t     required_capabilities;
    uint32_t     missing_capabilities;
} mission_validation_t;

void mission_plan_init_default(mission_plan_v1_t * p_plan);
app_status_t mission_plan_validate(mission_plan_v1_t const * p_plan,
                                   uint32_t capabilities,
                                   mission_validation_t * p_result);

#endif /* MISSION_PLAN_H_ */

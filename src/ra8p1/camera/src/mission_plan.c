#include "mission_plan.h"

#include <stdbool.h>
#include <stddef.h>

static bool mission_plan_in_range_u32(uint32_t value, uint32_t minimum, uint32_t maximum);

void mission_plan_init_default (mission_plan_v1_t * p_plan)
{
    if (NULL != p_plan)
    {
        *p_plan = (mission_plan_v1_t) {0};
        p_plan->schema_version          = MISSION_PLAN_SCHEMA_VERSION;
        p_plan->rows                    = 1U;
        p_plan->columns                 = 1U;
        p_plan->row_pitch_mm            = 300U;
        p_plan->cell_pitch_mm           = 300U;
        p_plan->capture_interval_mm     = 200U;
        p_plan->slide_position_count    = 1U;
        p_plan->slide_mode              = MISSION_SLIDE_FIXED;
        p_plan->slide_scan_distance_mm  = 300U;
        p_plan->slide_shot_interval_mm  = 50U;
        p_plan->settle_ms               = 800U;
        p_plan->max_speed_mm_s          = 200U;
        p_plan->move_timeout_ms         = 30000U;
        p_plan->inference_timeout_ms    = 5000U;
        p_plan->route_mode              = MISSION_ROUTE_SERPENTINE;
        p_plan->on_error_policy         = MISSION_ERROR_PAUSE;
    }
}

app_status_t mission_plan_validate (mission_plan_v1_t const * p_plan,
                                    uint32_t capabilities,
                                    mission_validation_t * p_result)
{
    uint32_t issue_mask = MISSION_ISSUE_NONE;
    uint32_t required = RA8P_CAPABILITY_MISSION_PLAN;
    uint32_t index;

    if ((NULL == p_plan) || (NULL == p_result))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (MISSION_PLAN_SCHEMA_VERSION != p_plan->schema_version)
    {
        issue_mask |= MISSION_ISSUE_SCHEMA_VERSION;
    }
    if (!mission_plan_in_range_u32(p_plan->rows, 1U, 100U))
    {
        issue_mask |= MISSION_ISSUE_ROWS;
    }
    if (!mission_plan_in_range_u32(p_plan->columns, 1U, 200U))
    {
        issue_mask |= MISSION_ISSUE_COLUMNS;
    }
    if (!mission_plan_in_range_u32(p_plan->row_pitch_mm, 10U, 10000U))
    {
        issue_mask |= MISSION_ISSUE_ROW_PITCH;
    }
    if (!mission_plan_in_range_u32(p_plan->cell_pitch_mm, 10U, 10000U))
    {
        issue_mask |= MISSION_ISSUE_CELL_PITCH;
    }
    if (!mission_plan_in_range_u32(p_plan->capture_interval_mm, 10U, 10000U))
    {
        issue_mask |= MISSION_ISSUE_CAPTURE_INTERVAL;
    }
    if (!mission_plan_in_range_u32(p_plan->travel_distance_mm, 0U, 1000000U))
    {
        issue_mask |= MISSION_ISSUE_TRAVEL_DISTANCE;
    }
    if (!mission_plan_in_range_u32(p_plan->slide_position_count, 1U,
                                   MISSION_PLAN_MAX_SLIDE_POSITIONS))
    {
        issue_mask |= MISSION_ISSUE_SLIDE_POSITIONS;
    }
    else
    {
        for (index = 0U; index < p_plan->slide_position_count; index++)
        {
            if ((p_plan->slide_positions_mm[index] < MISSION_PLAN_SLIDE_MIN_MM) ||
                (p_plan->slide_positions_mm[index] > MISSION_PLAN_SLIDE_MAX_MM))
            {
                issue_mask |= MISSION_ISSUE_SLIDE_POSITIONS;
            }
        }
    }
    if ((MISSION_SLIDE_FIXED != p_plan->slide_mode) &&
        (MISSION_SLIDE_SCAN != p_plan->slide_mode))
    {
        issue_mask |= MISSION_ISSUE_SLIDE_MODE;
    }
    if ((p_plan->slide_fixed_position_mm < MISSION_PLAN_SLIDE_MIN_MM) ||
        (p_plan->slide_fixed_position_mm > MISSION_PLAN_SLIDE_MAX_MM))
    {
        issue_mask |= MISSION_ISSUE_FIXED_POSITION;
    }
    if (!mission_plan_in_range_u32(p_plan->slide_scan_distance_mm, 0U,
                                   (uint32_t) MISSION_PLAN_SLIDE_MAX_MM) ||
        ((MISSION_SLIDE_SCAN == p_plan->slide_mode) &&
         !mission_plan_in_range_u32(p_plan->slide_scan_distance_mm, 10U,
                                    (uint32_t) MISSION_PLAN_SLIDE_MAX_MM)))
    {
        issue_mask |= MISSION_ISSUE_SLIDE_SCAN_DISTANCE;
    }
    if (!mission_plan_in_range_u32(p_plan->slide_shot_interval_mm, 0U,
                                   (uint32_t) MISSION_PLAN_SLIDE_MAX_MM) ||
        ((MISSION_SLIDE_SCAN == p_plan->slide_mode) &&
         !mission_plan_in_range_u32(p_plan->slide_shot_interval_mm, 10U,
                                    (uint32_t) MISSION_PLAN_SLIDE_MAX_MM)))
    {
        issue_mask |= MISSION_ISSUE_SLIDE_SHOT_INTERVAL;
    }
    if (!mission_plan_in_range_u32(p_plan->settle_ms, 0U, 30000U))
    {
        issue_mask |= MISSION_ISSUE_SETTLE_TIME;
    }
    if (!mission_plan_in_range_u32(p_plan->max_speed_mm_s, 1U, 3000U))
    {
        issue_mask |= MISSION_ISSUE_SPEED;
    }
    if (!mission_plan_in_range_u32(p_plan->move_timeout_ms, 1000U, 300000U))
    {
        issue_mask |= MISSION_ISSUE_MOVE_TIMEOUT;
    }
    if (!mission_plan_in_range_u32(p_plan->inference_timeout_ms, 100U, 120000U))
    {
        issue_mask |= MISSION_ISSUE_INFERENCE_TIMEOUT;
    }
    if ((MISSION_ROUTE_SERPENTINE != p_plan->route_mode) &&
        (MISSION_ROUTE_RASTER != p_plan->route_mode))
    {
        issue_mask |= MISSION_ISSUE_ROUTE_MODE;
    }
    if ((MISSION_ERROR_PAUSE != p_plan->on_error_policy) &&
        (MISSION_ERROR_ABORT != p_plan->on_error_policy) &&
        (MISSION_ERROR_RETRY_ONCE != p_plan->on_error_policy))
    {
        issue_mask |= MISSION_ISSUE_ERROR_POLICY;
    }
    if ((1U < p_plan->rows) || (1U < p_plan->columns) ||
        (0U < p_plan->travel_distance_mm))
    {
        required |= RA8P_CAPABILITY_MOTION;
    }
    if ((MISSION_SLIDE_SCAN == p_plan->slide_mode) ||
        (0 != p_plan->slide_fixed_position_mm))
    {
        required |= RA8P_CAPABILITY_SLIDE;
    }
    p_result->required_capabilities = required;
    p_result->missing_capabilities  = required & ~capabilities;
    p_result->issue_mask = issue_mask;
    if (0U != p_result->missing_capabilities)
    {
        p_result->issue_mask |= MISSION_ISSUE_CAPABILITY;
        p_result->status = APP_STATUS_UNAVAILABLE;
    }
    else if (MISSION_ISSUE_NONE != issue_mask)
    {
        p_result->status = APP_STATUS_INVALID_ARGUMENT;
    }
    else
    {
        p_result->status = APP_STATUS_OK;
    }
    return p_result->status;
}

static bool mission_plan_in_range_u32 (uint32_t value, uint32_t minimum, uint32_t maximum)
{
    return (value >= minimum) && (value <= maximum);
}

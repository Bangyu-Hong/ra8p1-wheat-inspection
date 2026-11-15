#ifndef WHEAT_POSTPROCESS_H_
#define WHEAT_POSTPROCESS_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "wheat_preprocess.h"

#define WHEAT_POSTPROCESS_MAX_CANDIDATES    (300U)
/* The frozen tile224 qualification evaluates up to 300 detections per frame.
 * Keep the result container at the same bound so a dense frame is not
 * invalidated after a successful bounded global NMS pass. */
#define WHEAT_POSTPROCESS_MAX_RESULTS       WHEAT_POSTPROCESS_MAX_CANDIDATES

typedef struct st_wheat_candidate
{
    float center_x;
    float center_y;
    float width;
    float height;
    float score;
} wheat_candidate_t;

typedef struct st_wheat_detection
{
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
    float    score;
} wheat_detection_t;

typedef struct st_wheat_postprocess_config
{
    float    confidence_threshold;
    float    iou_threshold;
    uint16_t source_width;
    uint16_t source_height;
} wheat_postprocess_config_t;

typedef struct st_wheat_detection_result
{
    wheat_detection_t detections[WHEAT_POSTPROCESS_MAX_RESULTS];
    uint32_t           candidates_scanned;
    uint32_t           candidates_above_threshold;
    uint32_t           candidates_dropped;
    uint32_t           detections_dropped;
    uint16_t           count;
    bool               result_overflow;
} wheat_detection_result_t;

app_status_t wheat_postprocess_candidates(wheat_candidate_t const * p_candidates,
                                           uint32_t candidate_count,
                                           wheat_preprocess_geometry_t const * p_geometry,
                                           wheat_postprocess_config_t const * p_config,
                                           wheat_detection_result_t * p_result);

#endif /* WHEAT_POSTPROCESS_H_ */

#ifndef WHEAT_YOLOX_DECODE_H_
#define WHEAT_YOLOX_DECODE_H_

#include <stdint.h>

#include "app_status.h"
#include "wheat_postprocess.h"

#define WHEAT_YOLOX_TILE_SIZE          (224U)
#define WHEAT_YOLOX_TILE_COUNT         (12U)
#define WHEAT_YOLOX_TILE_STRIDE        (179U)
#define WHEAT_YOLOX_OUTPUT_ROWS        (1029U)
#define WHEAT_YOLOX_OUTPUT_VALUES      (6U)
#define WHEAT_YOLOX_OUTPUT_BYTES       (WHEAT_YOLOX_OUTPUT_ROWS * WHEAT_YOLOX_OUTPUT_VALUES)
#define WHEAT_YOLOX_CONFIDENCE         (0.25F)
#define WHEAT_YOLOX_NMS_IOU            (0.45F)

typedef struct st_wheat_yolox_accumulator
{
    wheat_candidate_t candidates[WHEAT_POSTPROCESS_MAX_CANDIDATES];
    uint32_t           candidate_count;
    uint32_t           rows_scanned;
    uint32_t           above_threshold;
    uint32_t           candidates_dropped;
} wheat_yolox_accumulator_t;

app_status_t wheat_yolox_tile_origin(uint32_t tile_index,
                                      uint16_t * p_tile_x,
                                      uint16_t * p_tile_y);
void wheat_yolox_accumulator_init(wheat_yolox_accumulator_t * p_accumulator);
app_status_t wheat_yolox_accumulator_add_tile(wheat_yolox_accumulator_t * p_accumulator,
                                               int8_t const * p_output,
                                               uint32_t output_bytes,
                                               uint16_t tile_x,
                                               uint16_t tile_y);
app_status_t wheat_yolox_accumulator_finalize(wheat_yolox_accumulator_t const * p_accumulator,
                                               uint16_t frame_width,
                                               uint16_t frame_height,
                                               wheat_detection_result_t * p_result);
#endif /* WHEAT_YOLOX_DECODE_H_ */

#include "wheat_yolox_decode.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define WHEAT_YOLOX_OUTPUT_SCALE       (0.03774220868945122F)
#define WHEAT_YOLOX_OUTPUT_ZERO_POINT  (-48)

static uint16_t const g_tile_x[] = {0U, 179U, 358U, 416U};
static uint16_t const g_tile_y[] = {0U, 179U, 256U};

static float wheat_yolox_dequantize(int8_t value);
static void  wheat_yolox_grid(uint32_t row,
                              uint16_t * p_grid_x,
                              uint16_t * p_grid_y,
                              uint16_t * p_stride);
static void  wheat_yolox_keep_candidate(wheat_yolox_accumulator_t * p_accumulator,
                                         wheat_candidate_t const * p_candidate);

app_status_t wheat_yolox_tile_origin (uint32_t tile_index,
                                      uint16_t * p_tile_x,
                                      uint16_t * p_tile_y)
{
    uint32_t const columns = (uint32_t) (sizeof(g_tile_x) / sizeof(g_tile_x[0]));
    uint32_t const rows = (uint32_t) (sizeof(g_tile_y) / sizeof(g_tile_y[0]));

    if ((NULL == p_tile_x) || (NULL == p_tile_y) ||
        (tile_index >= (columns * rows)) ||
        ((columns * rows) != WHEAT_YOLOX_TILE_COUNT))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_tile_x = g_tile_x[tile_index % columns];
    *p_tile_y = g_tile_y[tile_index / columns];
    return APP_STATUS_OK;
}

void wheat_yolox_accumulator_init (wheat_yolox_accumulator_t * p_accumulator)
{
    if (NULL != p_accumulator)
    {
        (void) memset(p_accumulator, 0, sizeof(*p_accumulator));
    }
}

app_status_t wheat_yolox_accumulator_add_tile (wheat_yolox_accumulator_t * p_accumulator,
                                                int8_t const * p_output,
                                                uint32_t output_bytes,
                                                uint16_t tile_x,
                                                uint16_t tile_y)
{
    if ((NULL == p_accumulator) || (NULL == p_output) ||
        (WHEAT_YOLOX_OUTPUT_BYTES != output_bytes))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    for (uint32_t row = 0U; row < WHEAT_YOLOX_OUTPUT_ROWS; row++)
    {
        uint32_t const offset = row * WHEAT_YOLOX_OUTPUT_VALUES;
        float const objectness = wheat_yolox_dequantize(p_output[offset + 4U]);
        float const class_probability = wheat_yolox_dequantize(p_output[offset + 5U]);
        float score;
        uint16_t grid_x;
        uint16_t grid_y;
        uint16_t stride;
        wheat_candidate_t candidate;

        p_accumulator->rows_scanned++;
        /* Objectness and class are probabilities in the exported YOLOX head.
         * The shared tensor quantizer can still decode unused/sentinel bytes to
         * negative values; reject those before multiplying so two negatives do
         * not become a false high-confidence detection. */
        if (!isfinite(objectness) || !isfinite(class_probability) ||
            !(objectness > 0.0F) || !(class_probability > 0.0F))
        {
            continue;
        }
        /* Quantization can decode a sigmoid value near one slightly above
         * one (the output scale is shared with the box channels). */
        score = ((objectness > 1.0F) ? 1.0F : objectness) *
                ((class_probability > 1.0F) ? 1.0F : class_probability);
        if (!(score >= WHEAT_YOLOX_CONFIDENCE))
        {
            continue;
        }

        wheat_yolox_grid(row, &grid_x, &grid_y, &stride);
        candidate.center_x = ((wheat_yolox_dequantize(p_output[offset]) + grid_x) * stride) + tile_x;
        candidate.center_y = ((wheat_yolox_dequantize(p_output[offset + 1U]) + grid_y) * stride) + tile_y;
        candidate.width = expf(wheat_yolox_dequantize(p_output[offset + 2U])) * stride;
        candidate.height = expf(wheat_yolox_dequantize(p_output[offset + 3U])) * stride;
        candidate.score = score;

        if (!isfinite(candidate.center_x) || !isfinite(candidate.center_y) ||
            !isfinite(candidate.width) || !isfinite(candidate.height) ||
            !(candidate.width > 0.0F) || !(candidate.height > 0.0F))
        {
            continue;
        }

        p_accumulator->above_threshold++;
        wheat_yolox_keep_candidate(p_accumulator, &candidate);
    }

    return APP_STATUS_OK;
}

app_status_t wheat_yolox_accumulator_finalize (wheat_yolox_accumulator_t const * p_accumulator,
                                                uint16_t frame_width,
                                                uint16_t frame_height,
                                                wheat_detection_result_t * p_result)
{
    wheat_preprocess_geometry_t geometry;
    wheat_postprocess_config_t config;
    app_status_t status;

    if ((NULL == p_accumulator) || (NULL == p_result) ||
        (0U == frame_width) || (0U == frame_height))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    geometry = (wheat_preprocess_geometry_t)
    {
        .source_width  = frame_width,
        .source_height = frame_height,
        .target_width  = frame_width,
        .target_height = frame_height,
        .scaled_width  = frame_width,
        .scaled_height = frame_height,
        .offset_x      = 0U,
        .offset_y      = 0U,
        .output_bytes  = (uint32_t) frame_width * frame_height * 3U,
    };
    config = (wheat_postprocess_config_t)
    {
        .confidence_threshold = WHEAT_YOLOX_CONFIDENCE,
        .iou_threshold        = WHEAT_YOLOX_NMS_IOU,
        .source_width         = frame_width,
        .source_height        = frame_height,
    };
    status = wheat_postprocess_candidates(p_accumulator->candidates,
                                           p_accumulator->candidate_count,
                                           &geometry,
                                           &config,
                                           p_result);
    if (APP_STATUS_OK == status)
    {
        p_result->candidates_scanned = p_accumulator->rows_scanned;
        p_result->candidates_above_threshold = p_accumulator->above_threshold;
        p_result->candidates_dropped += p_accumulator->candidates_dropped;
        p_result->result_overflow = p_result->result_overflow ||
                                    (0U != p_accumulator->candidates_dropped);
    }

    return status;
}

static float wheat_yolox_dequantize (int8_t value)
{
    return ((float) value - (float) WHEAT_YOLOX_OUTPUT_ZERO_POINT) *
           WHEAT_YOLOX_OUTPUT_SCALE;
}

static void wheat_yolox_grid (uint32_t row,
                              uint16_t * p_grid_x,
                              uint16_t * p_grid_y,
                              uint16_t * p_stride)
{
    uint32_t local_row;
    uint16_t grid_width;

    if (row < (28U * 28U))
    {
        local_row = row;
        grid_width = 28U;
        *p_stride = 8U;
    }
    else if (row < ((28U * 28U) + (14U * 14U)))
    {
        local_row = row - (28U * 28U);
        grid_width = 14U;
        *p_stride = 16U;
    }
    else
    {
        local_row = row - (28U * 28U) - (14U * 14U);
        grid_width = 7U;
        *p_stride = 32U;
    }

    *p_grid_x = (uint16_t) (local_row % grid_width);
    *p_grid_y = (uint16_t) (local_row / grid_width);
}

static void wheat_yolox_keep_candidate (wheat_yolox_accumulator_t * p_accumulator,
                                        wheat_candidate_t const * p_candidate)
{
    if (p_accumulator->candidate_count < WHEAT_POSTPROCESS_MAX_CANDIDATES)
    {
        p_accumulator->candidates[p_accumulator->candidate_count] = *p_candidate;
        p_accumulator->candidate_count++;
        return;
    }

    {
        uint32_t minimum_index = 0U;

        for (uint32_t index = 1U; index < p_accumulator->candidate_count; index++)
        {
            if (p_accumulator->candidates[index].score <
                p_accumulator->candidates[minimum_index].score)
            {
                minimum_index = index;
            }
        }
        if (p_candidate->score > p_accumulator->candidates[minimum_index].score)
        {
            p_accumulator->candidates[minimum_index] = *p_candidate;
        }
        p_accumulator->candidates_dropped++;
    }
}

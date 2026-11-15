#include "wheat_postprocess.h"

#include <stdbool.h>
#include <float.h>
#include <stddef.h>

typedef struct st_wheat_float_box
{
    float x1;
    float y1;
    float x2;
    float y2;
    float score;
} wheat_float_box_t;

static wheat_float_box_t g_selected[WHEAT_POSTPROCESS_MAX_CANDIDATES];
static bool              g_suppressed[WHEAT_POSTPROCESS_MAX_CANDIDATES];

static void wheat_postprocess_map(wheat_candidate_t const * p_candidate,
                                  wheat_preprocess_geometry_t const * p_geometry,
                                  wheat_postprocess_config_t const * p_config,
                                  wheat_float_box_t * p_box);
static float wheat_postprocess_iou(wheat_float_box_t const * p_first,
                                   wheat_float_box_t const * p_second);
static void wheat_postprocess_sort(wheat_float_box_t * p_boxes, uint32_t count);
static uint16_t wheat_postprocess_round_coordinate(float value, uint16_t maximum);
static bool wheat_postprocess_is_finite(float value);

app_status_t wheat_postprocess_candidates (wheat_candidate_t const * p_candidates,
                                            uint32_t candidate_count,
                                            wheat_preprocess_geometry_t const * p_geometry,
                                            wheat_postprocess_config_t const * p_config,
                                            wheat_detection_result_t * p_result)
{
    uint32_t selected_count = 0U;

    if ((NULL == p_candidates) || (NULL == p_geometry) ||
        (NULL == p_config) || (NULL == p_result) ||
        (0U == p_config->source_width) || (0U == p_config->source_height) ||
        (p_geometry->source_width != p_config->source_width) ||
        (p_geometry->source_height != p_config->source_height) ||
        (0U == p_geometry->target_width) || (0U == p_geometry->target_height) ||
        (0U == p_geometry->scaled_width) || (0U == p_geometry->scaled_height) ||
        (p_geometry->scaled_width > p_geometry->target_width) ||
        (p_geometry->scaled_height > p_geometry->target_height) ||
        (((uint32_t) p_geometry->offset_x + p_geometry->scaled_width) >
         p_geometry->target_width) ||
        (((uint32_t) p_geometry->offset_y + p_geometry->scaled_height) >
         p_geometry->target_height) ||
        !wheat_postprocess_is_finite(p_config->confidence_threshold) ||
        !wheat_postprocess_is_finite(p_config->iou_threshold) ||
        (p_config->confidence_threshold < 0.0F) ||
        (p_config->confidence_threshold > 1.0F) ||
        (p_config->iou_threshold < 0.0F) || (p_config->iou_threshold > 1.0F))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_result = (wheat_detection_result_t) {0};
    p_result->candidates_scanned = candidate_count;
    for (uint32_t index = 0U; index < candidate_count; index++)
    {
        wheat_candidate_t const * const p_candidate = &p_candidates[index];
        wheat_float_box_t mapped;

        if (!wheat_postprocess_is_finite(p_candidate->center_x) ||
            !wheat_postprocess_is_finite(p_candidate->center_y) ||
            !wheat_postprocess_is_finite(p_candidate->width) ||
            !wheat_postprocess_is_finite(p_candidate->height) ||
            !wheat_postprocess_is_finite(p_candidate->score) ||
            (p_candidate->score < p_config->confidence_threshold) ||
            (p_candidate->width <= 0.0F) || (p_candidate->height <= 0.0F))
        {
            continue;
        }

        p_result->candidates_above_threshold++;
        wheat_postprocess_map(p_candidate, p_geometry, p_config, &mapped);
        if ((mapped.x2 <= mapped.x1) || (mapped.y2 <= mapped.y1))
        {
            continue;
        }

        if (selected_count < WHEAT_POSTPROCESS_MAX_CANDIDATES)
        {
            g_selected[selected_count] = mapped;
            selected_count++;
        }
        else
        {
            uint32_t minimum_index = 0U;

            for (uint32_t candidate_index = 1U;
                 candidate_index < selected_count;
                 candidate_index++)
            {
                if (g_selected[candidate_index].score < g_selected[minimum_index].score)
                {
                    minimum_index = candidate_index;
                }
            }
            if (p_candidate->score > g_selected[minimum_index].score)
            {
                g_selected[minimum_index] = mapped;
            }
            p_result->candidates_dropped++;
        }
    }

    wheat_postprocess_sort(g_selected, selected_count);
    for (uint32_t index = 0U; index < selected_count; index++)
    {
        g_suppressed[index] = false;
    }
    for (uint32_t index = 0U; index < selected_count; index++)
    {
        if (g_suppressed[index] ||
            (g_selected[index].x2 <= g_selected[index].x1) ||
            (g_selected[index].y2 <= g_selected[index].y1))
        {
            continue;
        }

        if (p_result->count < WHEAT_POSTPROCESS_MAX_RESULTS)
        {
            wheat_detection_t * const p_detection = &p_result->detections[p_result->count];

            *p_detection = (wheat_detection_t)
            {
                .x1 = wheat_postprocess_round_coordinate(g_selected[index].x1,
                                                          p_config->source_width),
                .y1 = wheat_postprocess_round_coordinate(g_selected[index].y1,
                                                          p_config->source_height),
                .x2 = wheat_postprocess_round_coordinate(g_selected[index].x2,
                                                          p_config->source_width),
                .y2 = wheat_postprocess_round_coordinate(g_selected[index].y2,
                                                          p_config->source_height),
                .score = g_selected[index].score,
            };
            p_result->count++;
        }
        else
        {
            p_result->detections_dropped++;
            p_result->result_overflow = true;
        }

        for (uint32_t candidate = index + 1U; candidate < selected_count; candidate++)
        {
            if (!g_suppressed[candidate] &&
                (g_selected[candidate].x2 > g_selected[candidate].x1) &&
                (g_selected[candidate].y2 > g_selected[candidate].y1) &&
                (wheat_postprocess_iou(&g_selected[index], &g_selected[candidate]) >
                 p_config->iou_threshold))
            {
                g_suppressed[candidate] = true;
            }
        }
    }

    if (p_result->result_overflow)
    {
        p_result->count = 0U;
        return APP_STATUS_DATA;
    }

    return APP_STATUS_OK;
}

static void wheat_postprocess_map (wheat_candidate_t const * p_candidate,
                                   wheat_preprocess_geometry_t const * p_geometry,
                                   wheat_postprocess_config_t const * p_config,
                                   wheat_float_box_t * p_box)
{
    float const scale_x = (float) p_config->source_width / (float) p_geometry->scaled_width;
    float const scale_y = (float) p_config->source_height / (float) p_geometry->scaled_height;
    float const half_width = p_candidate->width * 0.5F;
    float const half_height = p_candidate->height * 0.5F;
    wheat_float_box_t box =
    {
        .x1 = (p_candidate->center_x - half_width - (float) p_geometry->offset_x) * scale_x,
        .y1 = (p_candidate->center_y - half_height - (float) p_geometry->offset_y) * scale_y,
        .x2 = (p_candidate->center_x + half_width - (float) p_geometry->offset_x) * scale_x,
        .y2 = (p_candidate->center_y + half_height - (float) p_geometry->offset_y) * scale_y,
        .score = p_candidate->score,
    };

    if (box.x1 < 0.0F)
    {
        box.x1 = 0.0F;
    }
    if (box.y1 < 0.0F)
    {
        box.y1 = 0.0F;
    }
    if (box.x2 > (float) p_config->source_width)
    {
        box.x2 = (float) p_config->source_width;
    }
    if (box.y2 > (float) p_config->source_height)
    {
        box.y2 = (float) p_config->source_height;
    }

    *p_box = box;
}

static float wheat_postprocess_iou (wheat_float_box_t const * p_first,
                                    wheat_float_box_t const * p_second)
{
    float const left = (p_first->x1 > p_second->x1) ? p_first->x1 : p_second->x1;
    float const top = (p_first->y1 > p_second->y1) ? p_first->y1 : p_second->y1;
    float const right = (p_first->x2 < p_second->x2) ? p_first->x2 : p_second->x2;
    float const bottom = (p_first->y2 < p_second->y2) ? p_first->y2 : p_second->y2;
    float const intersection_width = (right > left) ? (right - left) : 0.0F;
    float const intersection_height = (bottom > top) ? (bottom - top) : 0.0F;
    float const intersection = intersection_width * intersection_height;
    float const first_area = (p_first->x2 - p_first->x1) * (p_first->y2 - p_first->y1);
    float const second_area = (p_second->x2 - p_second->x1) * (p_second->y2 - p_second->y1);
    float const union_area = first_area + second_area - intersection;

    return (union_area > 0.0F) ? (intersection / union_area) : 0.0F;
}

static void wheat_postprocess_sort (wheat_float_box_t * p_boxes, uint32_t count)
{
    for (uint32_t index = 1U; index < count; index++)
    {
        wheat_float_box_t const value = p_boxes[index];
        uint32_t position = index;

        while ((position > 0U) && (p_boxes[position - 1U].score < value.score))
        {
            p_boxes[position] = p_boxes[position - 1U];
            position--;
        }
        p_boxes[position] = value;
    }
}

static uint16_t wheat_postprocess_round_coordinate (float value, uint16_t maximum)
{
    if (value <= 0.0F)
    {
        return 0U;
    }
    if (value >= (float) maximum)
    {
        return maximum;
    }

    return (uint16_t) (value + 0.5F);
}

static bool wheat_postprocess_is_finite (float value)
{
    return (value >= -FLT_MAX) && (value <= FLT_MAX);
}

#include "tile_inference.h"

#include "app_config.h"
#include "wheat_ai.h"

static float minimum_float(float a, float b)
{
    return (a < b) ? a : b;
}

static float maximum_float(float a, float b)
{
    return (a > b) ? a : b;
}

static float detection_iou(const detection_t * a, const detection_t * b)
{
    float left = maximum_float(a->x, b->x);
    float top = maximum_float(a->y, b->y);
    float right = minimum_float(a->x + a->width, b->x + b->width);
    float bottom = minimum_float(a->y + a->height, b->y + b->height);
    float intersection_width = maximum_float(0.0f, right - left);
    float intersection_height = maximum_float(0.0f, bottom - top);
    float intersection = intersection_width * intersection_height;
    float union_area = (a->width * a->height) + (b->width * b->height) - intersection;

    return (union_area > 0.0f) ? (intersection / union_area) : 0.0f;
}

static uint16_t next_origin(uint16_t current, uint16_t tile_size, uint16_t overlap, uint16_t limit)
{
    uint16_t next;
    if ((uint32_t) current + tile_size >= limit)
    {
        return current;
    }
    next = (uint16_t) (current + tile_size - overlap);
    if ((uint32_t) next + tile_size > limit)
    {
        next = (uint16_t) (limit - tile_size);
    }
    return next;
}

static void map_detection_to_frame(detection_t * detection,
                                   image_rect_t tile,
                                   uint16_t frame_width,
                                   uint16_t frame_height)
{
    float scale_x = (float) tile.width / (float) MODEL_INPUT_WIDTH;
    float scale_y = (float) tile.height / (float) MODEL_INPUT_HEIGHT;
    float right;
    float bottom;

    detection->x = tile.x + (detection->x * scale_x);
    detection->y = tile.y + (detection->y * scale_y);
    detection->width *= scale_x;
    detection->height *= scale_y;

    detection->x = maximum_float(0.0f, detection->x);
    detection->y = maximum_float(0.0f, detection->y);
    right = minimum_float((float) frame_width, detection->x + detection->width);
    bottom = minimum_float((float) frame_height, detection->y + detection->height);
    detection->width = maximum_float(0.0f, right - detection->x);
    detection->height = maximum_float(0.0f, bottom - detection->y);
}

size_t tile_global_nms(detection_t * detections, size_t count, float iou_threshold)
{
    bool suppressed[APP_MAX_DETECTIONS] = {false};
    size_t index;
    size_t compare;
    size_t output_count = 0U;

    if ((detections == NULL) || (count > APP_MAX_DETECTIONS))
    {
        return 0U;
    }

    /* Sort high confidence first so each retained box suppresses weaker copies. */
    for (index = 1U; index < count; ++index)
    {
        detection_t key = detections[index];
        size_t position = index;
        while ((position > 0U) && (detections[position - 1U].score < key.score))
        {
            detections[position] = detections[position - 1U];
            --position;
        }
        detections[position] = key;
    }

    for (index = 0U; index < count; ++index)
    {
        if (suppressed[index])
        {
            continue;
        }
        detections[output_count++] = detections[index];
        for (compare = index + 1U; compare < count; ++compare)
        {
            if (!suppressed[compare] &&
                (detections[index].class_id == detections[compare].class_id) &&
                (detection_iou(&detections[index], &detections[compare]) >= iou_threshold))
            {
                suppressed[compare] = true;
            }
        }
    }

    return output_count;
}

app_status_t tile_inference_run(const image_t * frame,
                                detection_t * global_detections,
                                size_t capacity,
                                size_t * count)
{
    detection_t local[TILE_MAX_LOCAL_DETECTIONS];
    uint16_t tile_y = 0U;
    size_t global_count = 0U;

    if ((frame == NULL) || (global_detections == NULL) || (count == NULL) ||
        (frame->width < TILE_WIDTH) || (frame->height < TILE_HEIGHT) ||
        (TILE_OVERLAP >= TILE_WIDTH) || (TILE_OVERLAP >= TILE_HEIGHT) ||
        (capacity > APP_MAX_DETECTIONS))
    {
        return APP_ERR_ARGUMENT;
    }
    *count = 0U;

    for (;;)
    {
        uint16_t tile_x = 0U;
        for (;;)
        {
            image_rect_t tile = {tile_x, tile_y, TILE_WIDTH, TILE_HEIGHT};
            size_t local_count = 0U;
            size_t index;
            app_status_t status = wheat_ai_infer_tile(frame,
                                                      tile,
                                                      local,
                                                      TILE_MAX_LOCAL_DETECTIONS,
                                                      &local_count);
            if (status != APP_OK)
            {
                return status;
            }

            for (index = 0U; index < local_count; ++index)
            {
                if (global_count >= capacity)
                {
                    return APP_ERR_NO_SPACE;
                }
                map_detection_to_frame(&local[index], tile, frame->width, frame->height);
                if ((local[index].width > 0.0f) && (local[index].height > 0.0f))
                {
                    global_detections[global_count++] = local[index];
                }
            }

            {
                uint16_t next_x = next_origin(tile_x, TILE_WIDTH, TILE_OVERLAP, frame->width);
                if (next_x == tile_x)
                {
                    break;
                }
                tile_x = next_x;
            }
        }

        {
            uint16_t next_y = next_origin(tile_y, TILE_HEIGHT, TILE_OVERLAP, frame->height);
            if (next_y == tile_y)
            {
                break;
            }
            tile_y = next_y;
        }
    }

    *count = tile_global_nms(global_detections, global_count, GLOBAL_NMS_IOU_THRESHOLD);
    return APP_OK;
}

#include "wheat_ai.h"

#include "app_config.h"
#include "wheat_model_port.h"

static int8_t g_model_input[MODEL_INPUT_WIDTH * MODEL_INPUT_HEIGHT * MODEL_INPUT_CHANNELS];

static uint8_t expand_5_to_8(uint16_t value)
{
    return (uint8_t) ((value << 3U) | (value >> 2U));
}

static uint8_t expand_6_to_8(uint16_t value)
{
    return (uint8_t) ((value << 2U) | (value >> 4U));
}

static app_status_t preprocess_rgb565(const image_t * frame, image_rect_t tile)
{
    uint32_t output_x;
    uint32_t output_y;

    if ((frame == NULL) || (frame->data == NULL) || (frame->format != PIXEL_FORMAT_RGB565) ||
        (tile.width == 0U) || (tile.height == 0U) ||
        ((uint32_t) tile.x + tile.width > frame->width) ||
        ((uint32_t) tile.y + tile.height > frame->height))
    {
        return APP_ERR_ARGUMENT;
    }

    for (output_y = 0U; output_y < MODEL_INPUT_HEIGHT; ++output_y)
    {
        uint32_t source_y = tile.y + ((output_y * tile.height) / MODEL_INPUT_HEIGHT);
        const uint8_t * source_row = frame->data + ((size_t) source_y * frame->stride_bytes);

        for (output_x = 0U; output_x < MODEL_INPUT_WIDTH; ++output_x)
        {
            uint32_t source_x = tile.x + ((output_x * tile.width) / MODEL_INPUT_WIDTH);
            const uint8_t * pixel = source_row + ((size_t) source_x * 2U);
            uint16_t rgb565 = (uint16_t) pixel[0] | ((uint16_t) pixel[1] << 8U);
            size_t output_index = ((size_t) output_y * MODEL_INPUT_WIDTH + output_x) * 3U;
            uint8_t red = expand_5_to_8((rgb565 >> 11U) & 0x1FU);
            uint8_t green = expand_6_to_8((rgb565 >> 5U) & 0x3FU);
            uint8_t blue = expand_5_to_8(rgb565 & 0x1FU);

            /* Tensor contract: RGB uint8 range represented as INT8 -128..127. */
            g_model_input[output_index] = (int8_t) ((int16_t) red - 128);
            g_model_input[output_index + 1U] = (int8_t) ((int16_t) green - 128);
            g_model_input[output_index + 2U] = (int8_t) ((int16_t) blue - 128);
        }
    }

    return APP_OK;
}

app_status_t wheat_ai_init(void)
{
    return wheat_model_port_init();
}

app_status_t wheat_ai_infer_tile(const image_t * frame,
                                 image_rect_t tile,
                                 detection_t * detections,
                                 size_t capacity,
                                 size_t * count)
{
    model_detection_t model_output[TILE_MAX_LOCAL_DETECTIONS];
    size_t model_count = 0U;
    size_t accepted = 0U;
    size_t index;
    app_status_t status;

    if ((detections == NULL) || (count == NULL))
    {
        return APP_ERR_ARGUMENT;
    }
    *count = 0U;

    status = preprocess_rgb565(frame, tile);
    if (status != APP_OK)
    {
        return status;
    }

    status = wheat_model_port_invoke(g_model_input,
                                     sizeof(g_model_input),
                                     model_output,
                                     TILE_MAX_LOCAL_DETECTIONS,
                                     &model_count);
    if (status != APP_OK)
    {
        return APP_ERR_MODEL;
    }
    if (model_count > TILE_MAX_LOCAL_DETECTIONS)
    {
        return APP_ERR_PROTOCOL;
    }

    for (index = 0U; index < model_count; ++index)
    {
        float left;
        float top;

        if ((model_output[index].score < MODEL_SCORE_THRESHOLD) ||
            (model_output[index].width <= 0.0f) ||
            (model_output[index].height <= 0.0f))
        {
            continue;
        }
        if (accepted >= capacity)
        {
            return APP_ERR_NO_SPACE;
        }

        left = model_output[index].center_x - (model_output[index].width * 0.5f);
        top = model_output[index].center_y - (model_output[index].height * 0.5f);
        detections[accepted].x = (left < 0.0f) ? 0.0f : left;
        detections[accepted].y = (top < 0.0f) ? 0.0f : top;
        detections[accepted].width = model_output[index].width;
        detections[accepted].height = model_output[index].height;
        detections[accepted].score = model_output[index].score;
        detections[accepted].class_id = model_output[index].class_id;
        ++accepted;
    }

    *count = accepted;
    return APP_OK;
}

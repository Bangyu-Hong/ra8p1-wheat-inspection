#include "wheat_preprocess.h"

#include <float.h>
#include <limits.h>
#include <stddef.h>

static int32_t wheat_preprocess_quantize(uint8_t value,
                                         wheat_preprocess_config_t const * p_config);
static uint32_t wheat_preprocess_index(uint16_t x,
                                       uint16_t y,
                                       uint8_t channel,
                                       wheat_preprocess_geometry_t const * p_geometry,
                                       wheat_tensor_layout_t layout);
static void wheat_preprocess_unpack_rgb565_be(uint8_t const * p_pixel,
                                              uint8_t * p_red,
                                              uint8_t * p_green,
                                              uint8_t * p_blue);

app_status_t wheat_preprocess_rgb565_be (uint8_t const * p_source,
                                         uint32_t source_bytes,
                                         uint16_t source_width,
                                         uint16_t source_height,
                                         uint16_t source_stride,
                                         wheat_preprocess_config_t const * p_config,
                                         uint8_t * p_destination,
                                         uint32_t destination_bytes,
                                         wheat_preprocess_geometry_t * p_geometry)
{
    uint64_t output_bytes;
    uint64_t source_required;
    uint64_t aspect_left;
    uint64_t aspect_right;
    int32_t quant_lut[256];
    wheat_preprocess_geometry_t geometry;

    if ((NULL == p_source) || (NULL == p_config) ||
        (NULL == p_destination) || (NULL == p_geometry) ||
        (0U == source_width) || (0U == source_height) ||
        ((uint32_t) source_stride < ((uint32_t) source_width * 2U)) ||
        (0U == p_config->target_width) || (0U == p_config->target_height) ||
        !(p_config->tensor_scale > 0.0F) || (p_config->tensor_scale > FLT_MAX) ||
        !(p_config->source_scale > 0.0F) || (p_config->source_scale > FLT_MAX) ||
        ((WHEAT_TENSOR_LAYOUT_NHWC != p_config->layout) &&
         (WHEAT_TENSOR_LAYOUT_NCHW != p_config->layout)) ||
        ((WHEAT_TENSOR_UINT8 != p_config->type) &&
         (WHEAT_TENSOR_INT8 != p_config->type)))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    source_required = (uint64_t) source_stride * (uint64_t) source_height;
    output_bytes = (uint64_t) p_config->target_width *
                   (uint64_t) p_config->target_height * 3ULL;
    if ((source_required > source_bytes) || (output_bytes > destination_bytes) ||
        (output_bytes > UINT32_MAX))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    aspect_left = (uint64_t) p_config->target_width * (uint64_t) source_height;
    aspect_right = (uint64_t) p_config->target_height * (uint64_t) source_width;
    geometry = (wheat_preprocess_geometry_t)
    {
        .source_width  = source_width,
        .source_height = source_height,
        .target_width  = p_config->target_width,
        .target_height = p_config->target_height,
        .output_bytes  = (uint32_t) output_bytes,
    };
    if (aspect_left <= aspect_right)
    {
        geometry.scaled_width = p_config->target_width;
        geometry.scaled_height = (uint16_t) (((uint32_t) source_height *
                                              p_config->target_width) / source_width);
    }
    else
    {
        geometry.scaled_height = p_config->target_height;
        geometry.scaled_width = (uint16_t) (((uint32_t) source_width *
                                             p_config->target_height) / source_height);
    }
    if ((0U == geometry.scaled_width) || (0U == geometry.scaled_height))
    {
        return APP_STATUS_DATA;
    }
    geometry.offset_x = (uint16_t) ((geometry.target_width - geometry.scaled_width) / 2U);
    geometry.offset_y = (uint16_t) ((geometry.target_height - geometry.scaled_height) / 2U);

    for (uint32_t value = 0U; value < 256U; value++)
    {
        quant_lut[value] = wheat_preprocess_quantize((uint8_t) value, p_config);
    }

    {
        uint8_t const padding = (uint8_t) quant_lut[p_config->padding_rgb];

        for (uint32_t index = 0U; index < geometry.output_bytes; index++)
        {
            p_destination[index] = padding;
        }
    }

    for (uint16_t destination_y = 0U;
         destination_y < geometry.scaled_height;
         destination_y++)
    {
        uint16_t const source_y = (uint16_t) (((uint32_t) destination_y * source_height) /
                                               geometry.scaled_height);
        uint8_t const * const p_source_row = &p_source[(uint32_t) source_y * source_stride];
        uint16_t const target_y = (uint16_t) (geometry.offset_y + destination_y);

        for (uint16_t destination_x = 0U;
             destination_x < geometry.scaled_width;
             destination_x++)
        {
            uint16_t const source_x = (uint16_t) (((uint32_t) destination_x * source_width) /
                                                   geometry.scaled_width);
            uint8_t const * const p_pixel = &p_source_row[(uint32_t) source_x * 2U];
            uint16_t const target_x = (uint16_t) (geometry.offset_x + destination_x);
            uint8_t red;
            uint8_t green;
            uint8_t blue;

            wheat_preprocess_unpack_rgb565_be(p_pixel, &red, &green, &blue);
            p_destination[wheat_preprocess_index(target_x,
                                                  target_y,
                                                  0U,
                                                  &geometry,
                                                  p_config->layout)] = (uint8_t) quant_lut[red];
            p_destination[wheat_preprocess_index(target_x,
                                                  target_y,
                                                  1U,
                                                  &geometry,
                                                  p_config->layout)] = (uint8_t) quant_lut[green];
            p_destination[wheat_preprocess_index(target_x,
                                                  target_y,
                                                  2U,
                                                  &geometry,
                                                  p_config->layout)] = (uint8_t) quant_lut[blue];
        }
    }

    *p_geometry = geometry;
    return APP_STATUS_OK;
}

app_status_t wheat_preprocess_rgb565_be_tile_bgr_int8 (uint8_t const * p_source,
                                                        uint32_t source_bytes,
                                                        uint16_t source_width,
                                                        uint16_t source_height,
                                                        uint16_t source_stride,
                                                        uint16_t tile_x,
                                                        uint16_t tile_y,
                                                        uint16_t tile_width,
                                                        uint16_t tile_height,
                                                        int8_t * p_destination,
                                                        uint32_t destination_bytes)
{
    uint64_t const source_required = (uint64_t) source_stride * source_height;
    uint64_t const output_required = (uint64_t) tile_width * tile_height * 3ULL;

    if ((NULL == p_source) || (NULL == p_destination) ||
        (0U == source_width) || (0U == source_height) ||
        (0U == tile_width) || (0U == tile_height) ||
        ((uint32_t) source_stride < ((uint32_t) source_width * 2U)) ||
        (((uint32_t) tile_x + tile_width) > source_width) ||
        (((uint32_t) tile_y + tile_height) > source_height) ||
        (source_required > source_bytes) ||
        (output_required > destination_bytes))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    for (uint16_t y = 0U; y < tile_height; y++)
    {
        uint8_t const * const p_row = &p_source[((uint32_t) tile_y + y) * source_stride];

        for (uint16_t x = 0U; x < tile_width; x++)
        {
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint32_t const destination_index = (((uint32_t) y * tile_width) + x) * 3U;
            uint8_t const * const p_pixel = &p_row[((uint32_t) tile_x + x) * 2U];

            wheat_preprocess_unpack_rgb565_be(p_pixel, &red, &green, &blue);
            p_destination[destination_index] = (int8_t) ((int32_t) blue - 128);
            p_destination[destination_index + 1U] = (int8_t) ((int32_t) green - 128);
            p_destination[destination_index + 2U] = (int8_t) ((int32_t) red - 128);
        }
    }

    return APP_STATUS_OK;
}

static int32_t wheat_preprocess_quantize (uint8_t value,
                                          wheat_preprocess_config_t const * p_config)
{
    float const transformed = (((float) value * p_config->source_scale) /
                               p_config->tensor_scale) +
                              (float) p_config->tensor_zero_point;
    float const minimum = (WHEAT_TENSOR_INT8 == p_config->type) ? -128.0F : 0.0F;
    float const maximum = (WHEAT_TENSOR_INT8 == p_config->type) ? 127.0F : 255.0F;
    float bounded = transformed;

    if (bounded < minimum)
    {
        bounded = minimum;
    }
    if (bounded > maximum)
    {
        bounded = maximum;
    }

    return (int32_t) ((bounded >= 0.0F) ? (bounded + 0.5F) : (bounded - 0.5F));
}

static uint32_t wheat_preprocess_index (uint16_t x,
                                        uint16_t y,
                                        uint8_t channel,
                                        wheat_preprocess_geometry_t const * p_geometry,
                                        wheat_tensor_layout_t layout)
{
    uint32_t const pixel = ((uint32_t) y * p_geometry->target_width) + x;

    if (WHEAT_TENSOR_LAYOUT_NCHW == layout)
    {
        uint32_t const plane = (uint32_t) p_geometry->target_width * p_geometry->target_height;

        return ((uint32_t) channel * plane) + pixel;
    }

    return (pixel * 3U) + channel;
}

static void wheat_preprocess_unpack_rgb565_be (uint8_t const * p_pixel,
                                               uint8_t * p_red,
                                               uint8_t * p_green,
                                               uint8_t * p_blue)
{
    uint16_t const pixel = (uint16_t) (((uint16_t) p_pixel[0] << 8U) | p_pixel[1]);
    uint8_t const red5 = (uint8_t) ((pixel >> 11U) & 0x1FU);
    uint8_t const green6 = (uint8_t) ((pixel >> 5U) & 0x3FU);
    uint8_t const blue5 = (uint8_t) (pixel & 0x1FU);

    *p_red = (uint8_t) ((red5 << 3U) | (red5 >> 2U));
    *p_green = (uint8_t) ((green6 << 2U) | (green6 >> 4U));
    *p_blue = (uint8_t) ((blue5 << 3U) | (blue5 >> 2U));
}

#ifndef WHEAT_PREPROCESS_H_
#define WHEAT_PREPROCESS_H_

#include <stdint.h>

#include "app_status.h"

typedef enum e_wheat_tensor_layout
{
    WHEAT_TENSOR_LAYOUT_NHWC = 0,
    WHEAT_TENSOR_LAYOUT_NCHW
} wheat_tensor_layout_t;

typedef enum e_wheat_tensor_type
{
    WHEAT_TENSOR_UINT8 = 0,
    WHEAT_TENSOR_INT8
} wheat_tensor_type_t;

typedef struct st_wheat_preprocess_config
{
    uint16_t              target_width;
    uint16_t              target_height;
    uint8_t               padding_rgb;
    wheat_tensor_layout_t layout;
    wheat_tensor_type_t   type;
    float                 source_scale;
    float                 tensor_scale;
    int32_t               tensor_zero_point;
} wheat_preprocess_config_t;

typedef struct st_wheat_preprocess_geometry
{
    uint16_t source_width;
    uint16_t source_height;
    uint16_t target_width;
    uint16_t target_height;
    uint16_t scaled_width;
    uint16_t scaled_height;
    uint16_t offset_x;
    uint16_t offset_y;
    uint32_t output_bytes;
} wheat_preprocess_geometry_t;

app_status_t wheat_preprocess_rgb565_be(uint8_t const * p_source,
                                        uint32_t source_bytes,
                                        uint16_t source_width,
                                        uint16_t source_height,
                                        uint16_t source_stride,
                                        wheat_preprocess_config_t const * p_config,
                                        uint8_t * p_destination,
                                        uint32_t destination_bytes,
                                        wheat_preprocess_geometry_t * p_geometry);

app_status_t wheat_preprocess_rgb565_be_tile_bgr_int8(uint8_t const * p_source,
                                                       uint32_t source_bytes,
                                                       uint16_t source_width,
                                                       uint16_t source_height,
                                                       uint16_t source_stride,
                                                       uint16_t tile_x,
                                                       uint16_t tile_y,
                                                       uint16_t tile_width,
                                                       uint16_t tile_height,
                                                       int8_t * p_destination,
                                                       uint32_t destination_bytes);

#endif /* WHEAT_PREPROCESS_H_ */

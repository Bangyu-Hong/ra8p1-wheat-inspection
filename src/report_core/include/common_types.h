#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    APP_OK = 0,
    APP_ERR_ARGUMENT,
    APP_ERR_TIMEOUT,
    APP_ERR_IO,
    APP_ERR_CHECKSUM,
    APP_ERR_NOT_READY,
    APP_ERR_NO_SPACE,
    APP_ERR_BAD_IMAGE,
    APP_ERR_MODEL,
    APP_ERR_PROTOCOL
} app_status_t;

typedef enum
{
    PIXEL_FORMAT_RGB565 = 0
} pixel_format_t;

typedef struct
{
    uint8_t * data;
    uint16_t width;
    uint16_t height;
    uint16_t stride_bytes;
    pixel_format_t format;
} image_t;

typedef struct
{
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} image_rect_t;

typedef struct
{
    float x;
    float y;
    float width;
    float height;
    float score;
    uint8_t class_id;
} detection_t;

typedef enum
{
    IMAGE_QUALITY_OK = 0,
    IMAGE_QUALITY_TOO_DARK,
    IMAGE_QUALITY_TOO_BRIGHT,
    IMAGE_QUALITY_BLURRED
} image_quality_code_t;

typedef struct
{
    image_quality_code_t code;
    float mean_luma;
    float dark_ratio;
    float bright_ratio;
    float sharpness;
} image_quality_t;

typedef struct
{
    int8_t temperature_c;
    uint8_t humidity_percent;
    bool valid;
} environment_t;

typedef struct
{
    char sample_id[24];
    uint32_t timestamp_s;
    uint16_t wheat_ear_count;
    environment_t environment;
    uint8_t light_percent;
    image_quality_t image_quality;
} detection_record_t;

#endif

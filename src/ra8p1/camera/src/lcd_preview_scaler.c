#include "lcd_preview_scaler.h"

#include <stddef.h>

#include "lcd_rgb565.h"

static uint16_t lcd_preview_scaler_read_rgb565_be(uint8_t const * p_source,
                                                   uint32_t offset);

app_status_t lcd_preview_scaler_scale_slice_rgb565_be (uint8_t const * p_source,
                                                        uint32_t source_bytes,
                                                        uint16_t source_width,
                                                        uint16_t source_height,
                                                        uint16_t source_stride,
                                                        uint16_t destination_first_row,
                                                        uint16_t destination_row_count,
                                                        uint16_t * p_destination,
                                                        uint32_t destination_capacity_pixels)
{
    uint32_t const source_row_bytes = (uint32_t) source_width * 2U;
    uint32_t const destination_pixels = (uint32_t) destination_row_count * LCD_PREVIEW_IMAGE_WIDTH;
    uint32_t required_source_bytes;

    if ((NULL == p_source) || (NULL == p_destination) ||
        (0U == source_width) || (0U == source_height) ||
        (source_width < LCD_PREVIEW_IMAGE_WIDTH) ||
        (source_height < LCD_PREVIEW_IMAGE_HEIGHT) ||
        (0U == destination_row_count) ||
        ((uint32_t) destination_first_row + destination_row_count > LCD_PREVIEW_IMAGE_HEIGHT) ||
        (destination_capacity_pixels < destination_pixels))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    required_source_bytes = ((uint32_t) source_height - 1U) * source_stride + source_row_bytes;
    if (((uint32_t) source_stride < source_row_bytes) || (source_bytes < required_source_bytes))
    {
        return APP_STATUS_DATA;
    }

    for (uint32_t slice_row = 0U; slice_row < destination_row_count; slice_row++)
    {
        uint32_t const destination_y = (uint32_t) destination_first_row + slice_row;
        uint32_t const source_y = (destination_y * source_height) /
                                  LCD_PREVIEW_IMAGE_HEIGHT;
        uint32_t const source_row_offset = source_y * source_stride;
        uint32_t const destination_row_offset = slice_row * LCD_PREVIEW_IMAGE_WIDTH;

        for (uint32_t destination_x = 0U;
             destination_x < LCD_PREVIEW_IMAGE_WIDTH;
             destination_x++)
        {
            uint32_t const source_x = (destination_x * source_width) /
                                      LCD_PREVIEW_IMAGE_WIDTH;
            uint32_t const source_offset = source_row_offset + (source_x * 2U);

            if (source_offset + 1U >= source_bytes)
            {
                return APP_STATUS_DATA;
            }

            p_destination[destination_row_offset + destination_x] =
                lcd_preview_scaler_read_rgb565_be(p_source, source_offset);
        }
    }

    return APP_STATUS_OK;
}

static uint16_t lcd_preview_scaler_read_rgb565_be (uint8_t const * p_source,
                                                    uint32_t offset)
{
    return lcd_rgb565_from_be_bytes(p_source[offset], p_source[offset + 1U]);
}

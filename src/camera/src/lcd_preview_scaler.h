#ifndef LCD_PREVIEW_SCALER_H_
#define LCD_PREVIEW_SCALER_H_

#include <stdint.h>

#include "app_status.h"

#define LCD_PREVIEW_IMAGE_WIDTH       (426U)
#define LCD_PREVIEW_IMAGE_HEIGHT      (320U)
#define LCD_PREVIEW_X_OFFSET          (27U)
#define LCD_PREVIEW_Y_OFFSET          (0U)
#define LCD_PREVIEW_SIDE_BAR_WIDTH    (27U)
#define LCD_PREVIEW_ROWS_PER_SLICE    (8U)
#define LCD_PREVIEW_SLICE_PIXELS      (LCD_PREVIEW_IMAGE_WIDTH * LCD_PREVIEW_ROWS_PER_SLICE)
#define LCD_PREVIEW_SCALER_NAME       "nearest-floor-scalar"

app_status_t lcd_preview_scaler_scale_slice_rgb565_be(uint8_t const * p_source,
                                                       uint32_t source_bytes,
                                                       uint16_t source_width,
                                                       uint16_t source_height,
                                                       uint16_t source_stride,
                                                       uint16_t destination_first_row,
                                                       uint16_t destination_row_count,
                                                       uint16_t * p_destination,
                                                       uint32_t destination_capacity_pixels);

#endif /* LCD_PREVIEW_SCALER_H_ */

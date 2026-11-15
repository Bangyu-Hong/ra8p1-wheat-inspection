#ifndef LCD_RESULT_OVERLAY_H_
#define LCD_RESULT_OVERLAY_H_

#include <stdint.h>

#include "app_status.h"
#include "wheat_postprocess.h"

typedef struct st_lcd_overlay_box
{
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} lcd_overlay_box_t;

app_status_t lcd_result_overlay_map_box(wheat_detection_t const * p_detection,
                                         lcd_overlay_box_t * p_box);
app_status_t lcd_result_overlay_draw(wheat_detection_result_t const * p_result,
                                      app_status_t inference_status);

#endif /* LCD_RESULT_OVERLAY_H_ */

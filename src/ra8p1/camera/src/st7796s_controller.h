#ifndef ST7796S_CONTROLLER_H_
#define ST7796S_CONTROLLER_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define ST7796S_LOGICAL_WIDTH     (480U)
#define ST7796S_LOGICAL_HEIGHT    (320U)

typedef struct st_st7796s_diagnostics
{
    uint32_t initialization_attempts;
    uint32_t commands_sent;
    uint32_t windows_set;
    uint32_t pixels_written;
    uint32_t errors;
    app_status_t last_status;
    bool initialized;
} st7796s_diagnostics_t;

app_status_t st7796s_controller_init(void);
app_status_t st7796s_controller_close(void);
app_status_t st7796s_controller_backlight_set(bool enabled);
app_status_t st7796s_controller_write_rect(uint16_t x,
                                           uint16_t y,
                                           uint16_t width,
                                           uint16_t height,
                                           uint16_t const * p_pixels);
app_status_t st7796s_controller_write_rect_be(uint16_t x,
                                              uint16_t y,
                                              uint16_t width,
                                              uint16_t height,
                                              uint8_t const * p_pixel_bytes);
app_status_t st7796s_controller_fill_rect(uint16_t x,
                                          uint16_t y,
                                          uint16_t width,
                                          uint16_t height,
                                          uint16_t color);
void         st7796s_controller_get_diagnostics(st7796s_diagnostics_t * p_diagnostics);

#endif /* ST7796S_CONTROLLER_H_ */

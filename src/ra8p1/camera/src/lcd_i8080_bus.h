#ifndef LCD_I8080_BUS_H_
#define LCD_I8080_BUS_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define LCD_I8080_STROBE_DELAY_ITERATIONS       (12U)
#define LCD_I8080_ST7796S_WRITE_CYCLE_MIN_NS    (66U)
#define LCD_I8080_ST7796S_CONTROL_PULSE_MIN_NS  (15U)
#define LCD_I8080_ST7796S_DATA_SETUP_HOLD_MIN_NS (10U)

typedef struct st_lcd_i8080_bus_diagnostics
{
    uint32_t command_transactions;
    uint32_t pixel_transactions;
    uint32_t bus_cycles;
    uint32_t pixels_written;
    uint32_t last_transfer_us;
    uint32_t max_transfer_us;
    uint32_t errors;
    uint32_t timing_errors;
    int32_t  last_fsp_error;
    uint16_t strobe_delay_iterations;
    uint16_t controller_write_cycle_min_ns;
    uint16_t controller_control_pulse_min_ns;
    uint16_t controller_data_setup_hold_min_ns;
    bool     initialized;
    bool     backlight_on;
} lcd_i8080_bus_diagnostics_t;

app_status_t lcd_i8080_bus_open(void);
app_status_t lcd_i8080_bus_close(void);
app_status_t lcd_i8080_bus_reset_set(bool asserted);
app_status_t lcd_i8080_bus_backlight_set(bool enabled);
app_status_t lcd_i8080_bus_write_command(uint8_t command,
                                         uint8_t const * p_parameters,
                                         uint32_t parameter_count);
app_status_t lcd_i8080_bus_write_pixels(uint16_t const * p_pixels, uint32_t pixel_count);
app_status_t lcd_i8080_bus_write_pixels_be(uint8_t const * p_pixel_bytes, uint32_t pixel_count);
app_status_t lcd_i8080_bus_fill(uint16_t pixel, uint32_t pixel_count);
void         lcd_i8080_bus_get_diagnostics(lcd_i8080_bus_diagnostics_t * p_diagnostics);

#endif /* LCD_I8080_BUS_H_ */

#include "lcd_i8080_bus.h"

#include <stddef.h>

#include "hal_data.h"
#include "platform_time.h"
#include "lcd_rgb565.h"

#define LCD_PIN_CS                         BSP_IO_PORT_06_PIN_01
#define LCD_PIN_RS                         BSP_IO_PORT_06_PIN_04
#define LCD_PIN_WR                         BSP_IO_PORT_06_PIN_05
#define LCD_PIN_RD                         BSP_IO_PORT_08_PIN_04
#define LCD_PIN_RESET                      BSP_IO_PORT_03_PIN_07
#define LCD_PIN_BACKLIGHT                  BSP_IO_PORT_03_PIN_08

#define LCD_CS_PIN_MASK                    (1UL << 1U)
#define LCD_RS_PIN_MASK                    (1UL << 4U)
#define LCD_WR_PIN_MASK                    (1UL << 5U)
#define LCD_WR_PIN_RESET_MASK              (LCD_WR_PIN_MASK << 16U)
typedef struct st_lcd_output_pin
{
    bsp_io_port_pin_t pin;
    bsp_io_level_t    initial_level;
    uint32_t          drive;
} lcd_output_pin_t;

static lcd_i8080_bus_diagnostics_t g_diagnostics;

static app_status_t lcd_i8080_configure_pin(lcd_output_pin_t const * p_pin);
static app_status_t lcd_i8080_set_control(bsp_io_port_pin_t pin, bsp_io_level_t level);
static void         lcd_i8080_record_transfer_time(uint32_t start_cycles);

static inline void lcd_i8080_strobe_delay (void)
{
    for (uint32_t index = 0U; index < LCD_I8080_STROBE_DELAY_ITERATIONS; index++)
    {
        __NOP();
    }
}

static inline void lcd_i8080_port_mask_write (R_PORT0_Type * p_port, uint16_t mask, uint16_t value)
{
    uint32_t const set_bits   = (uint32_t) (value & mask);
    uint32_t const reset_bits = ((uint32_t) ((uint16_t) (~value) & mask)) << 16U;

    p_port->PCNTR3 = set_bits | reset_bits;
}

static inline void lcd_i8080_write_cycle (uint16_t value)
{
    uint16_t const port_b = lcd_rgb565_port_b_value(value);
    uint16_t const port_7 = lcd_rgb565_port_7_value(value);
    uint16_t const port_8 = lcd_rgb565_port_8_value(value);

    lcd_i8080_port_mask_write(R_PORT11, LCD_RGB565_PORT_B_MASK, port_b);
    lcd_i8080_port_mask_write(R_PORT7, LCD_RGB565_PORT_7_MASK, port_7);
    lcd_i8080_port_mask_write(R_PORT8, LCD_RGB565_PORT_8_MASK, port_8);

    /* Device-memory writes are ordered; the delay also satisfies the ST7796S WR pulse budget. */
    R_PORT6->PCNTR3 = LCD_WR_PIN_RESET_MASK;
    lcd_i8080_strobe_delay();
    R_PORT6->PCNTR3 = LCD_WR_PIN_MASK;
    lcd_i8080_strobe_delay();
}

static inline void lcd_i8080_port6_control_write (uint32_t pin_mask, bool high)
{
    R_PORT6->PCNTR3 = high ? pin_mask : (pin_mask << 16U);
}

app_status_t lcd_i8080_bus_open (void)
{
    static lcd_output_pin_t const pins[] =
    {
        {BSP_IO_PORT_11_PIN_00, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_11_PIN_01, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_11_PIN_02, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_11_PIN_03, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_11_PIN_04, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_11_PIN_05, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_11_PIN_06, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_07_PIN_05, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_07_PIN_06, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_07_PIN_07, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_07_PIN_13, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_07_PIN_14, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_07_PIN_15, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_08_PIN_00, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_08_PIN_01, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {BSP_IO_PORT_08_PIN_02, BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_HS_HIGH},
        {LCD_PIN_CS,            BSP_IO_LEVEL_HIGH, IOPORT_CFG_DRIVE_MID},
        {LCD_PIN_RS,            BSP_IO_LEVEL_HIGH, IOPORT_CFG_DRIVE_MID},
        {LCD_PIN_WR,            BSP_IO_LEVEL_HIGH, IOPORT_CFG_DRIVE_HS_HIGH},
        {LCD_PIN_RD,            BSP_IO_LEVEL_HIGH, IOPORT_CFG_DRIVE_MID},
        {LCD_PIN_RESET,         BSP_IO_LEVEL_HIGH, IOPORT_CFG_DRIVE_MID},
        {LCD_PIN_BACKLIGHT,     BSP_IO_LEVEL_LOW,  IOPORT_CFG_DRIVE_MID},
    };

    if (g_diagnostics.initialized)
    {
        return APP_STATUS_OK;
    }

    g_diagnostics = (lcd_i8080_bus_diagnostics_t) {0};
    g_diagnostics.strobe_delay_iterations = LCD_I8080_STROBE_DELAY_ITERATIONS;
    g_diagnostics.controller_write_cycle_min_ns = LCD_I8080_ST7796S_WRITE_CYCLE_MIN_NS;
    g_diagnostics.controller_control_pulse_min_ns = LCD_I8080_ST7796S_CONTROL_PULSE_MIN_NS;
    g_diagnostics.controller_data_setup_hold_min_ns = LCD_I8080_ST7796S_DATA_SETUP_HOLD_MIN_NS;
    for (uint32_t index = 0U; index < (uint32_t) (sizeof(pins) / sizeof(pins[0])); index++)
    {
        app_status_t const status = lcd_i8080_configure_pin(&pins[index]);

        if (APP_STATUS_OK != status)
        {
            return status;
        }
    }

    g_diagnostics.initialized = true;
    return APP_STATUS_OK;
}

app_status_t lcd_i8080_bus_close (void)
{
    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_OK;
    }

    (void) lcd_i8080_bus_backlight_set(false);
    lcd_i8080_port6_control_write(LCD_CS_PIN_MASK, true);
    lcd_i8080_port6_control_write(LCD_RS_PIN_MASK, true);
    (void) lcd_i8080_set_control(LCD_PIN_RD, BSP_IO_LEVEL_HIGH);
    (void) lcd_i8080_set_control(LCD_PIN_WR, BSP_IO_LEVEL_HIGH);
    g_diagnostics.initialized = false;

    return APP_STATUS_OK;
}

app_status_t lcd_i8080_bus_reset_set (bool asserted)
{
    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    return lcd_i8080_set_control(LCD_PIN_RESET,
                                 asserted ? BSP_IO_LEVEL_LOW : BSP_IO_LEVEL_HIGH);
}

app_status_t lcd_i8080_bus_backlight_set (bool enabled)
{
    app_status_t status;

    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    status = lcd_i8080_set_control(LCD_PIN_BACKLIGHT,
                                   enabled ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
    if (APP_STATUS_OK == status)
    {
        g_diagnostics.backlight_on = enabled;
    }

    return status;
}

app_status_t lcd_i8080_bus_write_command (uint8_t command,
                                          uint8_t const * p_parameters,
                                          uint32_t parameter_count)
{
    uint32_t start_cycles;

    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if ((0U != parameter_count) && (NULL == p_parameters))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    start_cycles = platform_time_raw_cycles();
    lcd_i8080_port6_control_write(LCD_CS_PIN_MASK, false);
    lcd_i8080_port6_control_write(LCD_RS_PIN_MASK, false);
    lcd_i8080_write_cycle((uint16_t) command);
    lcd_i8080_port6_control_write(LCD_RS_PIN_MASK, true);
    for (uint32_t index = 0U; index < parameter_count; index++)
    {
        lcd_i8080_write_cycle((uint16_t) p_parameters[index]);
    }
    lcd_i8080_port6_control_write(LCD_CS_PIN_MASK, true);

    g_diagnostics.command_transactions++;
    g_diagnostics.bus_cycles += 1U + parameter_count;
    lcd_i8080_record_transfer_time(start_cycles);

    return APP_STATUS_OK;
}

app_status_t lcd_i8080_bus_write_pixels (uint16_t const * p_pixels, uint32_t pixel_count)
{
    uint32_t start_cycles;

    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if ((NULL == p_pixels) || (0U == pixel_count))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    start_cycles = platform_time_raw_cycles();
    lcd_i8080_port6_control_write(LCD_CS_PIN_MASK, false);
    lcd_i8080_port6_control_write(LCD_RS_PIN_MASK, true);
    for (uint32_t index = 0U; index < pixel_count; index++)
    {
        lcd_i8080_write_cycle(p_pixels[index]);
    }
    lcd_i8080_port6_control_write(LCD_CS_PIN_MASK, true);

    g_diagnostics.pixel_transactions++;
    g_diagnostics.bus_cycles += pixel_count;
    g_diagnostics.pixels_written += pixel_count;
    lcd_i8080_record_transfer_time(start_cycles);

    return APP_STATUS_OK;
}

app_status_t lcd_i8080_bus_write_pixels_be (uint8_t const * p_pixel_bytes, uint32_t pixel_count)
{
    uint32_t start_cycles;

    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if ((NULL == p_pixel_bytes) || (0U == pixel_count))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    start_cycles = platform_time_raw_cycles();
    lcd_i8080_port6_control_write(LCD_CS_PIN_MASK, false);
    lcd_i8080_port6_control_write(LCD_RS_PIN_MASK, true);
    for (uint32_t index = 0U; index < pixel_count; index++)
    {
        uint32_t const offset = index * 2U;
        uint16_t const pixel = lcd_rgb565_from_be_bytes(p_pixel_bytes[offset],
                                                        p_pixel_bytes[offset + 1U]);

        lcd_i8080_write_cycle(pixel);
    }
    lcd_i8080_port6_control_write(LCD_CS_PIN_MASK, true);

    g_diagnostics.pixel_transactions++;
    g_diagnostics.bus_cycles += pixel_count;
    g_diagnostics.pixels_written += pixel_count;
    lcd_i8080_record_transfer_time(start_cycles);

    return APP_STATUS_OK;
}

app_status_t lcd_i8080_bus_fill (uint16_t pixel, uint32_t pixel_count)
{
    uint32_t start_cycles;

    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if (0U == pixel_count)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    start_cycles = platform_time_raw_cycles();
    lcd_i8080_port6_control_write(LCD_CS_PIN_MASK, false);
    lcd_i8080_port6_control_write(LCD_RS_PIN_MASK, true);
    for (uint32_t index = 0U; index < pixel_count; index++)
    {
        lcd_i8080_write_cycle(pixel);
    }
    lcd_i8080_port6_control_write(LCD_CS_PIN_MASK, true);

    g_diagnostics.pixel_transactions++;
    g_diagnostics.bus_cycles += pixel_count;
    g_diagnostics.pixels_written += pixel_count;
    lcd_i8080_record_transfer_time(start_cycles);

    return APP_STATUS_OK;
}

void lcd_i8080_bus_get_diagnostics (lcd_i8080_bus_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        *p_diagnostics = g_diagnostics;
    }
}

static app_status_t lcd_i8080_configure_pin (lcd_output_pin_t const * p_pin)
{
    uint32_t const output_level = (BSP_IO_LEVEL_HIGH == p_pin->initial_level) ?
                                  (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH :
                                  (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW;
    fsp_err_t const error = g_ioport.p_api->pinCfg(g_ioport.p_ctrl,
                                                   p_pin->pin,
                                                   (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                                                   p_pin->drive |
                                                   output_level);

    if (FSP_SUCCESS != error)
    {
        g_diagnostics.errors++;
        g_diagnostics.last_fsp_error = (int32_t) error;
        return APP_STATUS_IO;
    }

    return APP_STATUS_OK;
}

static app_status_t lcd_i8080_set_control (bsp_io_port_pin_t pin, bsp_io_level_t level)
{
    fsp_err_t const error = g_ioport.p_api->pinWrite(g_ioport.p_ctrl, pin, level);

    if (FSP_SUCCESS != error)
    {
        g_diagnostics.errors++;
        g_diagnostics.last_fsp_error = (int32_t) error;
        return APP_STATUS_IO;
    }

    return APP_STATUS_OK;
}

static void lcd_i8080_record_transfer_time (uint32_t start_cycles)
{
    uint32_t transfer_us;

    if (APP_STATUS_OK == platform_time_elapsed_us(start_cycles,
                                                  platform_time_raw_cycles(),
                                                  &transfer_us))
    {
        g_diagnostics.last_transfer_us = transfer_us;
        if (transfer_us > g_diagnostics.max_transfer_us)
        {
            g_diagnostics.max_transfer_us = transfer_us;
        }
    }
    else
    {
        g_diagnostics.timing_errors++;
    }
}

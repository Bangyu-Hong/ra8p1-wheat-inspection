#include "st7796s_controller.h"

#include <stddef.h>

#include "hal_data.h"
#include "lcd_i8080_bus.h"

#define ST7796S_CMD_SLEEP_OUT            (0x11U)
#define ST7796S_CMD_INVERSION_ON         (0x21U)
#define ST7796S_CMD_DISPLAY_ON           (0x29U)
#define ST7796S_CMD_COLUMN_ADDRESS_SET   (0x2AU)
#define ST7796S_CMD_PAGE_ADDRESS_SET     (0x2BU)
#define ST7796S_CMD_MEMORY_WRITE         (0x2CU)
#define ST7796S_CMD_MEMORY_ACCESS        (0x36U)
#define ST7796S_CMD_PIXEL_FORMAT         (0x3AU)
#define ST7796S_MADCTL_LANDSCAPE_BGR     (0x28U)
#define ST7796S_PIXEL_FORMAT_RGB565      (0x55U)

typedef struct st_st7796s_init_command
{
    uint8_t  command;
    uint8_t  length;
    uint8_t  data[14];
    uint16_t delay_ms;
} st7796s_init_command_t;

static st7796s_diagnostics_t g_diagnostics;

static app_status_t st7796s_set_window(uint16_t x,
                                       uint16_t y,
                                       uint16_t width,
                                       uint16_t height);
static app_status_t st7796s_record(app_status_t status);

app_status_t st7796s_controller_init (void)
{
    static st7796s_init_command_t const init_commands[] =
    {
        {ST7796S_CMD_SLEEP_OUT,          0U,  {0}, 120U},
        {0xF0U,                          1U,  {0xC3U}, 0U},
        {0xF0U,                          1U,  {0x96U}, 0U},
        {ST7796S_CMD_MEMORY_ACCESS,      1U,  {ST7796S_MADCTL_LANDSCAPE_BGR}, 0U},
        {ST7796S_CMD_PIXEL_FORMAT,       1U,  {ST7796S_PIXEL_FORMAT_RGB565}, 0U},
        {0xB4U,                          1U,  {0x01U}, 0U},
        {0xB7U,                          1U,  {0xC6U}, 0U},
        {0xE8U,                          8U,  {0x40U, 0x8AU, 0x00U, 0x00U, 0x29U, 0x19U, 0xA5U, 0x33U}, 0U},
        {0xC1U,                          1U,  {0x06U}, 0U},
        {0xC2U,                          1U,  {0xA7U}, 0U},
        {0xC5U,                          1U,  {0x18U}, 0U},
        {0xE0U,                         14U,  {0xF0U, 0x09U, 0x0BU, 0x06U, 0x04U, 0x15U, 0x2FU,
                                               0x54U, 0x42U, 0x3CU, 0x17U, 0x14U, 0x18U, 0x1BU}, 0U},
        {0xE1U,                         14U,  {0xF0U, 0x09U, 0x0BU, 0x06U, 0x04U, 0x03U, 0x2DU,
                                               0x43U, 0x42U, 0x3BU, 0x16U, 0x14U, 0x17U, 0x1BU}, 0U},
        {0xF0U,                          1U,  {0x3CU}, 0U},
        {0xF0U,                          1U,  {0x69U}, 120U},
        {ST7796S_CMD_INVERSION_ON,       0U,  {0}, 0U},
        {ST7796S_CMD_DISPLAY_ON,         0U,  {0}, 20U},
    };
    app_status_t status;

    if (g_diagnostics.initialized)
    {
        return APP_STATUS_OK;
    }

    g_diagnostics = (st7796s_diagnostics_t) {0};
    g_diagnostics.initialization_attempts = 1U;
    status = lcd_i8080_bus_open();
    if (APP_STATUS_OK != status)
    {
        return st7796s_record(status);
    }

    (void) lcd_i8080_bus_backlight_set(false);
    R_BSP_SoftwareDelay(5U, BSP_DELAY_UNITS_MILLISECONDS);
    status = lcd_i8080_bus_reset_set(true);
    if (APP_STATUS_OK == status)
    {
        R_BSP_SoftwareDelay(20U, BSP_DELAY_UNITS_MILLISECONDS);
        status = lcd_i8080_bus_reset_set(false);
        R_BSP_SoftwareDelay(120U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    for (uint32_t index = 0U;
         (APP_STATUS_OK == status) &&
         (index < (uint32_t) (sizeof(init_commands) / sizeof(init_commands[0])));
         index++)
    {
        status = lcd_i8080_bus_write_command(init_commands[index].command,
                                              init_commands[index].data,
                                              init_commands[index].length);
        if (APP_STATUS_OK == status)
        {
            g_diagnostics.commands_sent++;
            if (0U != init_commands[index].delay_ms)
            {
                R_BSP_SoftwareDelay(init_commands[index].delay_ms, BSP_DELAY_UNITS_MILLISECONDS);
            }
        }
    }

    if (APP_STATUS_OK == status)
    {
        g_diagnostics.initialized = true;
    }

    return st7796s_record(status);
}

app_status_t st7796s_controller_close (void)
{
    (void) lcd_i8080_bus_backlight_set(false);
    g_diagnostics.initialized = false;
    return lcd_i8080_bus_close();
}

app_status_t st7796s_controller_backlight_set (bool enabled)
{
    if (!g_diagnostics.initialized)
    {
        return st7796s_record(APP_STATUS_NOT_INITIALIZED);
    }

    return st7796s_record(lcd_i8080_bus_backlight_set(enabled));
}

app_status_t st7796s_controller_write_rect (uint16_t x,
                                            uint16_t y,
                                            uint16_t width,
                                            uint16_t height,
                                            uint16_t const * p_pixels)
{
    app_status_t status;
    uint32_t     pixel_count;

    if (NULL == p_pixels)
    {
        return st7796s_record(APP_STATUS_INVALID_ARGUMENT);
    }

    status = st7796s_set_window(x, y, width, height);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    pixel_count = (uint32_t) width * (uint32_t) height;
    status = lcd_i8080_bus_write_pixels(p_pixels, pixel_count);
    if (APP_STATUS_OK == status)
    {
        g_diagnostics.pixels_written += pixel_count;
    }

    return st7796s_record(status);
}

app_status_t st7796s_controller_write_rect_be (uint16_t x,
                                                uint16_t y,
                                                uint16_t width,
                                                uint16_t height,
                                                uint8_t const * p_pixel_bytes)
{
    app_status_t status;
    uint32_t     pixel_count;

    if (NULL == p_pixel_bytes)
    {
        return st7796s_record(APP_STATUS_INVALID_ARGUMENT);
    }

    status = st7796s_set_window(x, y, width, height);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    pixel_count = (uint32_t) width * (uint32_t) height;
    status = lcd_i8080_bus_write_pixels_be(p_pixel_bytes, pixel_count);
    if (APP_STATUS_OK == status)
    {
        g_diagnostics.pixels_written += pixel_count;
    }

    return st7796s_record(status);
}

app_status_t st7796s_controller_fill_rect (uint16_t x,
                                           uint16_t y,
                                           uint16_t width,
                                           uint16_t height,
                                           uint16_t color)
{
    app_status_t status = st7796s_set_window(x, y, width, height);
    uint32_t     pixel_count;

    if (APP_STATUS_OK != status)
    {
        return status;
    }

    pixel_count = (uint32_t) width * (uint32_t) height;
    status = lcd_i8080_bus_fill(color, pixel_count);
    if (APP_STATUS_OK == status)
    {
        g_diagnostics.pixels_written += pixel_count;
    }

    return st7796s_record(status);
}

void st7796s_controller_get_diagnostics (st7796s_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        *p_diagnostics = g_diagnostics;
    }
}

static app_status_t st7796s_set_window (uint16_t x,
                                        uint16_t y,
                                        uint16_t width,
                                        uint16_t height)
{
    uint16_t x_end;
    uint16_t y_end;
    uint8_t  address[4];
    app_status_t status;

    if (!g_diagnostics.initialized || (0U == width) || (0U == height) ||
        (x >= ST7796S_LOGICAL_WIDTH) || (y >= ST7796S_LOGICAL_HEIGHT) ||
        ((uint32_t) x + width > ST7796S_LOGICAL_WIDTH) ||
        ((uint32_t) y + height > ST7796S_LOGICAL_HEIGHT))
    {
        return st7796s_record(g_diagnostics.initialized ?
                              APP_STATUS_INVALID_ARGUMENT : APP_STATUS_NOT_INITIALIZED);
    }

    x_end = (uint16_t) (x + width - 1U);
    y_end = (uint16_t) (y + height - 1U);
    address[0] = (uint8_t) (x >> 8U);
    address[1] = (uint8_t) x;
    address[2] = (uint8_t) (x_end >> 8U);
    address[3] = (uint8_t) x_end;
    status = lcd_i8080_bus_write_command(ST7796S_CMD_COLUMN_ADDRESS_SET, address, 4U);
    if (APP_STATUS_OK == status)
    {
        address[0] = (uint8_t) (y >> 8U);
        address[1] = (uint8_t) y;
        address[2] = (uint8_t) (y_end >> 8U);
        address[3] = (uint8_t) y_end;
        status = lcd_i8080_bus_write_command(ST7796S_CMD_PAGE_ADDRESS_SET, address, 4U);
    }
    if (APP_STATUS_OK == status)
    {
        status = lcd_i8080_bus_write_command(ST7796S_CMD_MEMORY_WRITE, NULL, 0U);
    }
    if (APP_STATUS_OK == status)
    {
        g_diagnostics.commands_sent += 3U;
        g_diagnostics.windows_set++;
    }

    return st7796s_record(status);
}

static app_status_t st7796s_record (app_status_t status)
{
    g_diagnostics.last_status = status;
    if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
    {
        g_diagnostics.errors++;
    }

    return status;
}

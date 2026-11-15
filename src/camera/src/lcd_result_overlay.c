#include "lcd_result_overlay.h"

#include <stddef.h>

#include "camera_sensor.h"
#include "lcd_preview_scaler.h"
#include "st7796s_controller.h"

#define LCD_OVERLAY_BOX_COLOR           (0x07E0U)
#define LCD_OVERLAY_WAIT_COLOR          (0xFFE0U)
#define LCD_OVERLAY_ERROR_COLOR         (0xF800U)
#define LCD_OVERLAY_TEXT_COLOR          (0xFFFFU)
#define LCD_OVERLAY_BACKGROUND_COLOR    (0x0000U)
#define LCD_OVERLAY_BOX_THICKNESS       (2U)
#define LCD_OVERLAY_LABEL_WIDTH         (122U)
#define LCD_OVERLAY_LABEL_HEIGHT        (20U)
#define LCD_OVERLAY_GLYPH_SCALE         (2U)
#define LCD_OVERLAY_GLYPH_WIDTH         (5U)
#define LCD_OVERLAY_GLYPH_HEIGHT        (7U)
#define LCD_OVERLAY_GLYPH_ADVANCE       (12U)

static uint16_t g_label_pixels[LCD_OVERLAY_LABEL_WIDTH * LCD_OVERLAY_LABEL_HEIGHT];

static app_status_t lcd_result_overlay_draw_box(lcd_overlay_box_t const * p_box);
static void         lcd_result_overlay_write_text(char const * p_text, uint16_t color);
static void         lcd_result_overlay_write_glyph(uint16_t origin_x,
                                                   char character,
                                                   uint16_t color);
static uint8_t      lcd_result_overlay_glyph_column(char character, uint8_t column);
static void         lcd_result_overlay_format_label(char * p_text,
                                                    uint32_t capacity,
                                                    uint16_t count,
                                                    app_status_t status);

app_status_t lcd_result_overlay_map_box (wheat_detection_t const * p_detection,
                                          lcd_overlay_box_t * p_box)
{
    uint32_t x1;
    uint32_t y1;
    uint32_t x2;
    uint32_t y2;

    if ((NULL == p_detection) || (NULL == p_box) ||
        (p_detection->x1 >= p_detection->x2) ||
        (p_detection->y1 >= p_detection->y2) ||
        (p_detection->x2 > CAMERA_SENSOR_VGA_WIDTH) ||
        (p_detection->y2 > CAMERA_SENSOR_VGA_HEIGHT))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    x1 = LCD_PREVIEW_X_OFFSET +
         (((uint32_t) p_detection->x1 * LCD_PREVIEW_IMAGE_WIDTH) / CAMERA_SENSOR_VGA_WIDTH);
    y1 = LCD_PREVIEW_Y_OFFSET +
         (((uint32_t) p_detection->y1 * LCD_PREVIEW_IMAGE_HEIGHT) / CAMERA_SENSOR_VGA_HEIGHT);
    x2 = LCD_PREVIEW_X_OFFSET +
         ((((uint32_t) p_detection->x2 * LCD_PREVIEW_IMAGE_WIDTH) +
           CAMERA_SENSOR_VGA_WIDTH - 1U) / CAMERA_SENSOR_VGA_WIDTH);
    y2 = LCD_PREVIEW_Y_OFFSET +
         ((((uint32_t) p_detection->y2 * LCD_PREVIEW_IMAGE_HEIGHT) +
           CAMERA_SENSOR_VGA_HEIGHT - 1U) / CAMERA_SENSOR_VGA_HEIGHT);

    if (x2 > (LCD_PREVIEW_X_OFFSET + LCD_PREVIEW_IMAGE_WIDTH))
    {
        x2 = LCD_PREVIEW_X_OFFSET + LCD_PREVIEW_IMAGE_WIDTH;
    }
    if (y2 > (LCD_PREVIEW_Y_OFFSET + LCD_PREVIEW_IMAGE_HEIGHT))
    {
        y2 = LCD_PREVIEW_Y_OFFSET + LCD_PREVIEW_IMAGE_HEIGHT;
    }

    *p_box = (lcd_overlay_box_t)
    {
        .x      = (uint16_t) x1,
        .y      = (uint16_t) y1,
        .width  = (uint16_t) (x2 - x1),
        .height = (uint16_t) (y2 - y1),
    };
    return ((0U == p_box->width) || (0U == p_box->height)) ? APP_STATUS_DATA : APP_STATUS_OK;
}

app_status_t lcd_result_overlay_draw (wheat_detection_result_t const * p_result,
                                       app_status_t inference_status)
{
    char label[11];
    uint16_t label_color;
    app_status_t status;

    if ((NULL == p_result) || (p_result->count > WHEAT_POSTPROCESS_MAX_RESULTS))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (APP_STATUS_OK == inference_status)
    {
        for (uint16_t index = 0U; index < p_result->count; index++)
        {
            lcd_overlay_box_t box;

            status = lcd_result_overlay_map_box(&p_result->detections[index], &box);
            if (APP_STATUS_OK == status)
            {
                status = lcd_result_overlay_draw_box(&box);
            }
            if (APP_STATUS_OK != status)
            {
                return status;
            }
        }
        label_color = LCD_OVERLAY_BOX_COLOR;
    }
    else if (APP_STATUS_UNAVAILABLE == inference_status)
    {
        label_color = LCD_OVERLAY_WAIT_COLOR;
    }
    else
    {
        label_color = LCD_OVERLAY_ERROR_COLOR;
    }

    lcd_result_overlay_format_label(label,
                                    (uint32_t) sizeof(label),
                                    p_result->count,
                                    inference_status);
    lcd_result_overlay_write_text(label, label_color);
    return st7796s_controller_write_rect(LCD_PREVIEW_X_OFFSET,
                                          LCD_PREVIEW_Y_OFFSET,
                                          LCD_OVERLAY_LABEL_WIDTH,
                                          LCD_OVERLAY_LABEL_HEIGHT,
                                          g_label_pixels);
}

static app_status_t lcd_result_overlay_draw_box (lcd_overlay_box_t const * p_box)
{
    uint16_t const horizontal_thickness = (p_box->height < LCD_OVERLAY_BOX_THICKNESS) ?
                                           p_box->height : LCD_OVERLAY_BOX_THICKNESS;
    uint16_t const vertical_thickness = (p_box->width < LCD_OVERLAY_BOX_THICKNESS) ?
                                         p_box->width : LCD_OVERLAY_BOX_THICKNESS;
    app_status_t status = st7796s_controller_fill_rect(p_box->x,
                                                       p_box->y,
                                                       p_box->width,
                                                       horizontal_thickness,
                                                       LCD_OVERLAY_BOX_COLOR);

    if ((APP_STATUS_OK == status) && (p_box->height > horizontal_thickness))
    {
        status = st7796s_controller_fill_rect(p_box->x,
                                              (uint16_t) (p_box->y + p_box->height -
                                                          horizontal_thickness),
                                              p_box->width,
                                              horizontal_thickness,
                                              LCD_OVERLAY_BOX_COLOR);
    }
    if (APP_STATUS_OK == status)
    {
        status = st7796s_controller_fill_rect(p_box->x,
                                              p_box->y,
                                              vertical_thickness,
                                              p_box->height,
                                              LCD_OVERLAY_BOX_COLOR);
    }
    if ((APP_STATUS_OK == status) && (p_box->width > vertical_thickness))
    {
        status = st7796s_controller_fill_rect((uint16_t) (p_box->x + p_box->width -
                                                          vertical_thickness),
                                              p_box->y,
                                              vertical_thickness,
                                              p_box->height,
                                              LCD_OVERLAY_BOX_COLOR);
    }

    return status;
}

static void lcd_result_overlay_write_text (char const * p_text, uint16_t color)
{
    uint16_t origin_x = 4U;

    for (uint32_t index = 0U;
         index < (LCD_OVERLAY_LABEL_WIDTH * LCD_OVERLAY_LABEL_HEIGHT);
         index++)
    {
        g_label_pixels[index] = LCD_OVERLAY_BACKGROUND_COLOR;
    }

    while (('\0' != *p_text) &&
           ((origin_x + (LCD_OVERLAY_GLYPH_WIDTH * LCD_OVERLAY_GLYPH_SCALE)) <
            LCD_OVERLAY_LABEL_WIDTH))
    {
        lcd_result_overlay_write_glyph(origin_x, *p_text, color);
        origin_x = (uint16_t) (origin_x + LCD_OVERLAY_GLYPH_ADVANCE);
        p_text++;
    }
}

static void lcd_result_overlay_write_glyph (uint16_t origin_x,
                                            char character,
                                            uint16_t color)
{
    for (uint8_t column = 0U; column < LCD_OVERLAY_GLYPH_WIDTH; column++)
    {
        uint8_t const bits = lcd_result_overlay_glyph_column(character, column);

        for (uint8_t row = 0U; row < LCD_OVERLAY_GLYPH_HEIGHT; row++)
        {
            if (0U != (bits & (uint8_t) (1U << row)))
            {
                for (uint8_t scale_y = 0U; scale_y < LCD_OVERLAY_GLYPH_SCALE; scale_y++)
                {
                    for (uint8_t scale_x = 0U; scale_x < LCD_OVERLAY_GLYPH_SCALE; scale_x++)
                    {
                        uint16_t const x = (uint16_t) (origin_x +
                                                       ((uint16_t) column * LCD_OVERLAY_GLYPH_SCALE) +
                                                       scale_x);
                        uint16_t const y = (uint16_t) (3U +
                                                       ((uint16_t) row * LCD_OVERLAY_GLYPH_SCALE) +
                                                       scale_y);

                        g_label_pixels[((uint32_t) y * LCD_OVERLAY_LABEL_WIDTH) + x] = color;
                    }
                }
            }
        }
    }
}

static uint8_t lcd_result_overlay_glyph_column (char character, uint8_t column)
{
    static uint8_t const digits[10][LCD_OVERLAY_GLYPH_WIDTH] =
    {
        {0x3EU, 0x51U, 0x49U, 0x45U, 0x3EU},
        {0x00U, 0x42U, 0x7FU, 0x40U, 0x00U},
        {0x62U, 0x51U, 0x49U, 0x49U, 0x46U},
        {0x22U, 0x49U, 0x49U, 0x49U, 0x36U},
        {0x18U, 0x14U, 0x12U, 0x7FU, 0x10U},
        {0x2FU, 0x49U, 0x49U, 0x49U, 0x31U},
        {0x3EU, 0x49U, 0x49U, 0x49U, 0x32U},
        {0x01U, 0x71U, 0x09U, 0x05U, 0x03U},
        {0x36U, 0x49U, 0x49U, 0x49U, 0x36U},
        {0x26U, 0x49U, 0x49U, 0x49U, 0x3EU},
    };
    uint8_t const * p_glyph = NULL;
    static uint8_t const glyph_a[] = {0x7EU, 0x11U, 0x11U, 0x11U, 0x7EU};
    static uint8_t const glyph_e[] = {0x7FU, 0x49U, 0x49U, 0x49U, 0x41U};
    static uint8_t const glyph_h[] = {0x7FU, 0x08U, 0x08U, 0x08U, 0x7FU};
    static uint8_t const glyph_i[] = {0x00U, 0x41U, 0x7FU, 0x41U, 0x00U};
    static uint8_t const glyph_f[] = {0x7FU, 0x09U, 0x09U, 0x09U, 0x01U};
    static uint8_t const glyph_o[] = {0x3EU, 0x41U, 0x41U, 0x41U, 0x3EU};
    static uint8_t const glyph_r[] = {0x7FU, 0x09U, 0x19U, 0x29U, 0x46U};
    static uint8_t const glyph_t[] = {0x01U, 0x01U, 0x7FU, 0x01U, 0x01U};
    static uint8_t const glyph_w[] = {0x3FU, 0x40U, 0x38U, 0x40U, 0x3FU};

    if ((character >= '0') && (character <= '9'))
    {
        p_glyph = digits[(uint32_t) (character - '0')];
    }
    else
    {
        switch (character)
        {
            case 'A': p_glyph = glyph_a; break;
            case 'E': p_glyph = glyph_e; break;
            case 'F': p_glyph = glyph_f; break;
            case 'H': p_glyph = glyph_h; break;
            case 'I': p_glyph = glyph_i; break;
            case 'O': p_glyph = glyph_o; break;
            case 'R': p_glyph = glyph_r; break;
            case 'T': p_glyph = glyph_t; break;
            case 'W': p_glyph = glyph_w; break;
            default: break;
        }
    }

    return (NULL == p_glyph) ? 0U : p_glyph[column];
}

static void lcd_result_overlay_format_label (char * p_text,
                                             uint32_t capacity,
                                             uint16_t count,
                                             app_status_t status)
{
    if ((NULL == p_text) || (capacity < 9U))
    {
        return;
    }

    if (APP_STATUS_OK == status)
    {
        uint16_t const bounded_count = (count > 999U) ? 999U : count;

        p_text[0] = 'W';
        p_text[1] = 'H';
        p_text[2] = 'E';
        p_text[3] = 'A';
        p_text[4] = 'T';
        p_text[5] = ' ';
        p_text[6] = (char) ('0' + ((bounded_count / 100U) % 10U));
        p_text[7] = (char) ('0' + ((bounded_count / 10U) % 10U));
        p_text[8] = (char) ('0' + (bounded_count % 10U));
        p_text[9] = '\0';
    }
    else if (APP_STATUS_UNAVAILABLE == status)
    {
        p_text[0] = 'A'; p_text[1] = 'I'; p_text[2] = ' ';
        p_text[3] = 'O'; p_text[4] = 'F'; p_text[5] = 'F';
        p_text[6] = '\0';
    }
    else
    {
        p_text[0] = 'A'; p_text[1] = 'I'; p_text[2] = ' ';
        p_text[3] = 'E'; p_text[4] = 'R'; p_text[5] = 'R';
        p_text[6] = '\0';
    }
}

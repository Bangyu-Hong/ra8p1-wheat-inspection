#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lcd_preview_scaler.h"

#define TEST_SOURCE_WIDTH     (640U)
#define TEST_SOURCE_HEIGHT    (480U)
#define TEST_SOURCE_STRIDE    (TEST_SOURCE_WIDTH * 2U)
#define TEST_SOURCE_BYTES     (TEST_SOURCE_STRIDE * TEST_SOURCE_HEIGHT)
#define TEST_OUTPUT_PIXELS    (LCD_PREVIEW_IMAGE_WIDTH * LCD_PREVIEW_IMAGE_HEIGHT)
#define TEST_SENTINEL         (0xDEADU)

#define TEST_CHECK(expression)                                                         \
    do                                                                                 \
    {                                                                                  \
        if (!(expression))                                                             \
        {                                                                              \
            (void) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression);       \
            return false;                                                              \
        }                                                                              \
    } while (0)

static uint8_t  g_source[TEST_SOURCE_BYTES];
static uint16_t g_full_output[TEST_OUTPUT_PIXELS];
static uint16_t g_sliced_output[TEST_OUTPUT_PIXELS];

static bool test_geometry(void);
static bool test_nearest_and_byte_order(void);
static bool test_complete_coverage(void);
static bool test_sliced_output(void);
static bool test_bounds(void);
static void source_fill(uint16_t pixel);
static void source_set_pixel(uint16_t x, uint16_t y, uint16_t pixel);

int main (void)
{
    typedef bool (* test_function_t)(void);
    typedef struct st_test_case
    {
        char const *    name;
        test_function_t function;
    } test_case_t;
    static test_case_t const tests[] =
    {
        {"geometry", test_geometry},
        {"nearest/byte-order", test_nearest_and_byte_order},
        {"complete coverage", test_complete_coverage},
        {"sliced output", test_sliced_output},
        {"bounds", test_bounds},
    };

    for (uint32_t index = 0U; index < (uint32_t) (sizeof(tests) / sizeof(tests[0])); index++)
    {
        if (!tests[index].function())
        {
            return 1;
        }

        (void) printf("PASS %s\n", tests[index].name);
    }

    (void) printf("lcd-preview-scaler status=ok source=640x480 destination=426x320 "
                  "bars=27/27 filter=%s\n",
                  LCD_PREVIEW_SCALER_NAME);
    return 0;
}

static bool test_geometry (void)
{
    TEST_CHECK(480U == (LCD_PREVIEW_X_OFFSET +
                        LCD_PREVIEW_IMAGE_WIDTH +
                        LCD_PREVIEW_SIDE_BAR_WIDTH));
    TEST_CHECK(320U == (LCD_PREVIEW_Y_OFFSET + LCD_PREVIEW_IMAGE_HEIGHT));
    TEST_CHECK(LCD_PREVIEW_X_OFFSET == LCD_PREVIEW_SIDE_BAR_WIDTH);
    TEST_CHECK(TEST_SOURCE_WIDTH > LCD_PREVIEW_IMAGE_WIDTH);
    TEST_CHECK(TEST_SOURCE_HEIGHT > LCD_PREVIEW_IMAGE_HEIGHT);

    return true;
}

static bool test_nearest_and_byte_order (void)
{
    uint32_t const center_index = (160U * LCD_PREVIEW_IMAGE_WIDTH) + 213U;

    source_fill(0x0000U);
    source_set_pixel(0U, 0U, 0xF800U);
    source_set_pixel(1U, 0U, 0x07E0U);
    source_set_pixel(0U, 1U, 0x001FU);
    source_set_pixel(1U, 1U, 0xFFFFU);
    source_set_pixel(320U, 240U, 0x07E0U);
    source_set_pixel(321U, 240U, 0x07E0U);
    source_set_pixel(320U, 241U, 0x07E0U);
    source_set_pixel(321U, 241U, 0x07E0U);
    source_set_pixel(638U, 478U, 0x001FU);
    source_set_pixel(639U, 478U, 0x001FU);
    source_set_pixel(638U, 479U, 0x001FU);
    source_set_pixel(639U, 479U, 0x001FU);

    TEST_CHECK(APP_STATUS_OK == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                         TEST_SOURCE_BYTES,
                                                                         TEST_SOURCE_WIDTH,
                                                                         TEST_SOURCE_HEIGHT,
                                                                         TEST_SOURCE_STRIDE,
                                                                         0U,
                                                                         LCD_PREVIEW_IMAGE_HEIGHT,
                                                                         g_full_output,
                                                                         TEST_OUTPUT_PIXELS));
    TEST_CHECK(0xF800U == g_full_output[0]);
    TEST_CHECK(0x07E0U == g_full_output[center_index]);
    TEST_CHECK(0x001FU == g_full_output[TEST_OUTPUT_PIXELS - 1U]);

    return true;
}

static bool test_complete_coverage (void)
{
    source_fill(0x1234U);
    for (uint32_t index = 0U; index < TEST_OUTPUT_PIXELS; index++)
    {
        g_full_output[index] = TEST_SENTINEL;
    }

    TEST_CHECK(APP_STATUS_OK == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                         TEST_SOURCE_BYTES,
                                                                         TEST_SOURCE_WIDTH,
                                                                         TEST_SOURCE_HEIGHT,
                                                                         TEST_SOURCE_STRIDE,
                                                                         0U,
                                                                         LCD_PREVIEW_IMAGE_HEIGHT,
                                                                         g_full_output,
                                                                         TEST_OUTPUT_PIXELS));
    for (uint32_t index = 0U; index < TEST_OUTPUT_PIXELS; index++)
    {
        TEST_CHECK(0x1234U == g_full_output[index]);
    }

    return true;
}

static bool test_sliced_output (void)
{
    source_fill(0xA55AU);
    (void) memset(g_sliced_output, 0, sizeof(g_sliced_output));
    TEST_CHECK(APP_STATUS_OK == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                         TEST_SOURCE_BYTES,
                                                                         TEST_SOURCE_WIDTH,
                                                                         TEST_SOURCE_HEIGHT,
                                                                         TEST_SOURCE_STRIDE,
                                                                         0U,
                                                                         LCD_PREVIEW_IMAGE_HEIGHT,
                                                                         g_full_output,
                                                                         TEST_OUTPUT_PIXELS));

    for (uint16_t row = 0U; row < LCD_PREVIEW_IMAGE_HEIGHT; row += LCD_PREVIEW_ROWS_PER_SLICE)
    {
        uint16_t const remaining_rows = (uint16_t) (LCD_PREVIEW_IMAGE_HEIGHT - row);
        uint16_t const rows = (remaining_rows > LCD_PREVIEW_ROWS_PER_SLICE) ?
                              LCD_PREVIEW_ROWS_PER_SLICE : remaining_rows;
        uint32_t const output_offset = (uint32_t) row * LCD_PREVIEW_IMAGE_WIDTH;

        TEST_CHECK(APP_STATUS_OK == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                             TEST_SOURCE_BYTES,
                                                                             TEST_SOURCE_WIDTH,
                                                                             TEST_SOURCE_HEIGHT,
                                                                             TEST_SOURCE_STRIDE,
                                                                             row,
                                                                             rows,
                                                                             &g_sliced_output[output_offset],
                                                                             TEST_OUTPUT_PIXELS - output_offset));
    }

    TEST_CHECK(0 == memcmp(g_full_output, g_sliced_output, sizeof(g_full_output)));
    return true;
}

static bool test_bounds (void)
{
    uint16_t destination[LCD_PREVIEW_IMAGE_WIDTH];

    source_fill(0xFFFFU);
    TEST_CHECK(APP_STATUS_DATA == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                           TEST_SOURCE_BYTES - 1U,
                                                                           TEST_SOURCE_WIDTH,
                                                                           TEST_SOURCE_HEIGHT,
                                                                           TEST_SOURCE_STRIDE,
                                                                           0U,
                                                                           1U,
                                                                           destination,
                                                                           LCD_PREVIEW_IMAGE_WIDTH));
    TEST_CHECK(APP_STATUS_DATA == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                           TEST_SOURCE_BYTES,
                                                                           TEST_SOURCE_WIDTH,
                                                                           TEST_SOURCE_HEIGHT,
                                                                           TEST_SOURCE_STRIDE - 1U,
                                                                           0U,
                                                                           1U,
                                                                           destination,
                                                                           LCD_PREVIEW_IMAGE_WIDTH));
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                                       TEST_SOURCE_BYTES,
                                                                                       TEST_SOURCE_WIDTH,
                                                                                       TEST_SOURCE_HEIGHT,
                                                                                       TEST_SOURCE_STRIDE,
                                                                                       LCD_PREVIEW_IMAGE_HEIGHT - 1U,
                                                                                       2U,
                                                                                       destination,
                                                                                       LCD_PREVIEW_IMAGE_WIDTH));
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                                       TEST_SOURCE_BYTES,
                                                                                       TEST_SOURCE_WIDTH,
                                                                                       TEST_SOURCE_HEIGHT,
                                                                                       TEST_SOURCE_STRIDE,
                                                                                       0U,
                                                                                       1U,
                                                                                       destination,
                                                                                       LCD_PREVIEW_IMAGE_WIDTH - 1U));
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                                       TEST_SOURCE_BYTES,
                                                                                       LCD_PREVIEW_IMAGE_WIDTH - 1U,
                                                                                       TEST_SOURCE_HEIGHT,
                                                                                       TEST_SOURCE_STRIDE,
                                                                                       0U,
                                                                                       1U,
                                                                                       destination,
                                                                                       LCD_PREVIEW_IMAGE_WIDTH));
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == lcd_preview_scaler_scale_slice_rgb565_be(g_source,
                                                                                       TEST_SOURCE_BYTES,
                                                                                       TEST_SOURCE_WIDTH,
                                                                                       LCD_PREVIEW_IMAGE_HEIGHT - 1U,
                                                                                       TEST_SOURCE_STRIDE,
                                                                                       0U,
                                                                                       1U,
                                                                                       destination,
                                                                                       LCD_PREVIEW_IMAGE_WIDTH));

    return true;
}

static void source_fill (uint16_t pixel)
{
    uint8_t const high = (uint8_t) (pixel >> 8U);
    uint8_t const low = (uint8_t) pixel;

    for (uint32_t offset = 0U; offset < TEST_SOURCE_BYTES; offset += 2U)
    {
        g_source[offset] = high;
        g_source[offset + 1U] = low;
    }
}

static void source_set_pixel (uint16_t x, uint16_t y, uint16_t pixel)
{
    uint32_t const offset = ((uint32_t) y * TEST_SOURCE_STRIDE) + ((uint32_t) x * 2U);

    g_source[offset] = (uint8_t) (pixel >> 8U);
    g_source[offset + 1U] = (uint8_t) pixel;
}

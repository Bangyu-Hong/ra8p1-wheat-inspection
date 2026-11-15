#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "app_status.h"
#include "camera_frame_pool.h"

#define CAMERA_CAPTURE_H_
#define CAMERA_SENSOR_H_
#define LCD_PREVIEW_SCALER_H_
#define PLATFORM_TIME_H_
#define ST7796S_CONTROLLER_H_

#define CAMERA_SENSOR_VGA_WIDTH           (640U)
#define CAMERA_SENSOR_VGA_HEIGHT          (480U)
#define CAMERA_FRAME_STRIDE               (1280U)
#define CAMERA_FRAME_BYTES                (614400U)
#define CAMERA_PIXEL_FORMAT_RGB565_BE     (0U)
#define LCD_PREVIEW_IMAGE_WIDTH           (426U)
#define LCD_PREVIEW_IMAGE_HEIGHT          (320U)
#define LCD_PREVIEW_X_OFFSET              (27U)
#define LCD_PREVIEW_Y_OFFSET              (0U)
#define LCD_PREVIEW_SIDE_BAR_WIDTH        (27U)
#define LCD_PREVIEW_ROWS_PER_SLICE        (8U)
#define LCD_PREVIEW_SLICE_PIXELS          (LCD_PREVIEW_IMAGE_WIDTH * LCD_PREVIEW_ROWS_PER_SLICE)
#define ST7796S_LOGICAL_WIDTH             (480U)
#define ST7796S_LOGICAL_HEIGHT            (320U)

app_status_t camera_capture_acquire_latest(camera_frame_lease_t * p_lease);
app_status_t camera_capture_release(camera_frame_lease_t const * p_lease);
app_status_t lcd_preview_scaler_scale_slice_rgb565_be(uint8_t const * p_source,
                                                       uint32_t source_bytes,
                                                       uint16_t source_width,
                                                       uint16_t source_height,
                                                       uint16_t source_stride,
                                                       uint16_t destination_first_row,
                                                       uint16_t destination_row_count,
                                                       uint16_t * p_destination,
                                                       uint32_t destination_capacity_pixels);
uint32_t platform_time_raw_cycles(void);
app_status_t platform_time_elapsed_us(uint32_t start_raw_cycles,
                                      uint32_t end_raw_cycles,
                                      uint32_t * p_elapsed_us);
app_status_t st7796s_controller_init(void);
app_status_t st7796s_controller_close(void);
app_status_t st7796s_controller_backlight_set(bool enabled);
app_status_t st7796s_controller_write_rect(uint16_t x,
                                           uint16_t y,
                                           uint16_t width,
                                           uint16_t height,
                                           uint16_t const * p_pixels);
app_status_t st7796s_controller_fill_rect(uint16_t x,
                                          uint16_t y,
                                          uint16_t width,
                                          uint16_t height,
                                          uint16_t color);

#include "../src/lcd_display_service.c"

#define TEST_CHECK(condition)                                                       \
    do                                                                              \
    {                                                                               \
        if (!(condition))                                                           \
        {                                                                           \
            (void) printf("check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                           \
        }                                                                           \
    } while (0)

static bool test_drive_to_live_wait(void);
static bool test_external_lease_ownership(void);
static bool test_external_lease_failure(void);

static uint8_t g_frame_bytes[4];
static camera_frame_t g_frame;
static camera_frame_lease_t g_external_lease;
static uint32_t g_release_calls;
static uint32_t g_raw_cycles;
static bool g_fail_write;

int main (void)
{
    TEST_CHECK(test_external_lease_ownership());
    TEST_CHECK(test_external_lease_failure());
    (void) printf("lcd-hold-service external-release=app-only retry=ok failure=ok\n");
    return 0;
}

static bool test_drive_to_live_wait (void)
{
    lcd_display_diagnostics_t diagnostics;

    g_frame = (camera_frame_t)
    {
        .sequence = 7U,
        .generation = 3U,
        .width = CAMERA_SENSOR_VGA_WIDTH,
        .height = CAMERA_SENSOR_VGA_HEIGHT,
        .stride = CAMERA_FRAME_STRIDE,
        .pixel_format = CAMERA_PIXEL_FORMAT_RGB565_BE,
        .valid_bytes = CAMERA_FRAME_BYTES,
        .capacity = CAMERA_FRAME_BYTES,
        .address = (uintptr_t) g_frame_bytes,
        .crc32 = 0x12345678UL,
        .status = APP_STATUS_OK,
    };
    g_external_lease = (camera_frame_lease_t)
    {
        .slot = 0U,
        .generation = g_frame.generation,
        .p_frame = &g_frame,
    };
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_init(0U));
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_process(2000000U));
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_process(2000000U));

    do
    {
        TEST_CHECK(APP_STATUS_OK == lcd_display_service_process(2000000U));
        lcd_display_service_get_diagnostics(&diagnostics);
    } while (LCD_DISPLAY_STATE_TRANSFER_STATIC_FRAME == diagnostics.state);

    TEST_CHECK(LCD_DISPLAY_STATE_STATIC_HOLD == diagnostics.state);
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_process(3000000U));
    TEST_CHECK(lcd_display_service_ready_for_hold());
    return true;
}

static bool test_external_lease_ownership (void)
{
    lcd_display_diagnostics_t diagnostics;
    uint32_t release_baseline;

    g_release_calls = 0U;
    g_raw_cycles = 0U;
    g_fail_write = false;
    TEST_CHECK(test_drive_to_live_wait());
    release_baseline = g_release_calls;
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_begin_hold(&g_external_lease));
    do
    {
        TEST_CHECK(APP_STATUS_OK == lcd_display_service_process(3000000U));
        lcd_display_service_get_diagnostics(&diagnostics);
    } while (LCD_DISPLAY_STATE_TRANSFER_HELD_FRAME == diagnostics.state);

    TEST_CHECK(lcd_display_service_hold_complete());
    TEST_CHECK(release_baseline == g_release_calls);
    TEST_CHECK(1U == diagnostics.external_holds_started);
    TEST_CHECK(1U == diagnostics.external_holds_completed);
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_resume_live());
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_resume_live());
    TEST_CHECK(release_baseline == g_release_calls);
    lcd_display_service_get_diagnostics(&diagnostics);
    TEST_CHECK(1U == diagnostics.external_hold_resumes);

    TEST_CHECK(APP_STATUS_OK == lcd_display_service_begin_frame_hold(&g_frame));
    do
    {
        TEST_CHECK(APP_STATUS_OK == lcd_display_service_process(3000000U));
        lcd_display_service_get_diagnostics(&diagnostics);
    } while (LCD_DISPLAY_STATE_TRANSFER_HELD_FRAME == diagnostics.state);
    TEST_CHECK(lcd_display_service_hold_complete());
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_resume_live());
    TEST_CHECK(release_baseline == g_release_calls);
    lcd_display_service_get_diagnostics(&diagnostics);
    TEST_CHECK(2U == diagnostics.external_holds_started);
    TEST_CHECK(2U == diagnostics.external_holds_completed);
    TEST_CHECK(2U == diagnostics.external_hold_resumes);

    TEST_CHECK(APP_STATUS_OK == lcd_display_service_begin_hold(&g_external_lease));
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_close());
    TEST_CHECK(release_baseline == g_release_calls);
    return true;
}

static bool test_external_lease_failure (void)
{
    lcd_display_diagnostics_t diagnostics;
    uint32_t release_baseline;

    g_fail_write = false;
    TEST_CHECK(test_drive_to_live_wait());
    release_baseline = g_release_calls;
    TEST_CHECK(APP_STATUS_OK == lcd_display_service_begin_hold(&g_external_lease));
    g_fail_write = true;
    TEST_CHECK(APP_STATUS_IO == lcd_display_service_process(3000000U));
    TEST_CHECK(release_baseline == g_release_calls);
    lcd_display_service_get_diagnostics(&diagnostics);
    TEST_CHECK(LCD_DISPLAY_STATE_ERROR == diagnostics.state);
    TEST_CHECK(1U == diagnostics.external_hold_failures);
    return true;
}

app_status_t camera_capture_acquire_latest (camera_frame_lease_t * p_lease)
{
    if (NULL == p_lease)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    *p_lease = g_external_lease;
    return APP_STATUS_OK;
}

app_status_t camera_capture_release (camera_frame_lease_t const * p_lease)
{
    if (NULL == p_lease)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    g_release_calls++;
    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_validate_lease (camera_frame_lease_t const * p_lease)
{
    return ((NULL != p_lease) && (p_lease->p_frame == &g_frame)) ?
           APP_STATUS_OK : APP_STATUS_DATA;
}

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
    (void) p_source;
    (void) source_bytes;
    (void) source_width;
    (void) source_height;
    (void) source_stride;
    (void) destination_first_row;
    (void) destination_row_count;
    (void) p_destination;
    (void) destination_capacity_pixels;
    return APP_STATUS_OK;
}

uint32_t platform_time_raw_cycles (void)
{
    g_raw_cycles += 10U;
    return g_raw_cycles;
}

app_status_t platform_time_elapsed_us (uint32_t start_raw_cycles,
                                       uint32_t end_raw_cycles,
                                       uint32_t * p_elapsed_us)
{
    if (NULL == p_elapsed_us)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    *p_elapsed_us = end_raw_cycles - start_raw_cycles;
    return APP_STATUS_OK;
}

app_status_t st7796s_controller_init (void)
{
    return APP_STATUS_OK;
}

app_status_t st7796s_controller_close (void)
{
    return APP_STATUS_OK;
}

app_status_t st7796s_controller_backlight_set (bool enabled)
{
    (void) enabled;
    return APP_STATUS_OK;
}

app_status_t st7796s_controller_write_rect (uint16_t x,
                                             uint16_t y,
                                             uint16_t width,
                                             uint16_t height,
                                             uint16_t const * p_pixels)
{
    (void) x;
    (void) y;
    (void) width;
    (void) height;
    (void) p_pixels;
    return g_fail_write ? APP_STATUS_IO : APP_STATUS_OK;
}

app_status_t st7796s_controller_fill_rect (uint16_t x,
                                            uint16_t y,
                                            uint16_t width,
                                            uint16_t height,
                                            uint16_t color)
{
    (void) x;
    (void) y;
    (void) width;
    (void) height;
    (void) color;
    return APP_STATUS_OK;
}

#include "lcd_display_service.h"

#include <stddef.h>

#include "camera_capture.h"
#include "camera_sensor.h"
#include "lcd_preview_scaler.h"
#include "platform_time.h"
#include "st7796s_controller.h"

#define LCD_COLOR_BAR_HOLD_US       (2000000ULL)
#define LCD_STATIC_FRAME_HOLD_US    (1000000ULL)

#define LCD_DISPLAY_STATIC_ASSERT(expression, name) \
    typedef char lcd_display_static_assert_ ## name[(expression) ? 1 : -1]

LCD_DISPLAY_STATIC_ASSERT((LCD_PREVIEW_X_OFFSET + LCD_PREVIEW_IMAGE_WIDTH +
                           LCD_PREVIEW_SIDE_BAR_WIDTH) == ST7796S_LOGICAL_WIDTH,
                          preview_width_matches_panel);
LCD_DISPLAY_STATIC_ASSERT((LCD_PREVIEW_Y_OFFSET + LCD_PREVIEW_IMAGE_HEIGHT) ==
                          ST7796S_LOGICAL_HEIGHT,
                          preview_height_matches_panel);
LCD_DISPLAY_STATIC_ASSERT(LCD_PREVIEW_X_OFFSET == LCD_PREVIEW_SIDE_BAR_WIDTH,
                          preview_side_bars_are_equal);

static lcd_display_diagnostics_t g_diagnostics;
static camera_frame_lease_t      g_lease;
static uint64_t                  g_stage_deadline_us;
static uint32_t                  g_frame_start_cycles;
static uint16_t                  g_preview_slice[LCD_PREVIEW_SLICE_PIXELS];

static app_status_t lcd_display_draw_color_bars(void);
static app_status_t lcd_display_acquire(lcd_display_state_t transfer_state);
static app_status_t lcd_display_transfer_slice(uint64_t now_us, bool static_frame);
static app_status_t lcd_display_validate_frame(camera_frame_t const * p_frame);
static app_status_t lcd_display_fail(app_status_t status, uint64_t now_us);

app_status_t lcd_display_service_init (uint64_t now_us)
{
    app_status_t status;

    if (g_diagnostics.initialized)
    {
        return APP_STATUS_OK;
    }

    g_diagnostics = (lcd_display_diagnostics_t) {0};
    g_lease = (camera_frame_lease_t) {0};
    status = st7796s_controller_init();
    if (APP_STATUS_OK == status)
    {
        status = lcd_display_draw_color_bars();
    }
    if (APP_STATUS_OK == status)
    {
        status = st7796s_controller_backlight_set(true);
    }
    if (APP_STATUS_OK != status)
    {
        return lcd_display_fail(status, now_us);
    }

    g_stage_deadline_us      = now_us + LCD_COLOR_BAR_HOLD_US;
    g_diagnostics.state      = LCD_DISPLAY_STATE_COLOR_BARS;
    g_diagnostics.initialized = true;
    g_diagnostics.last_status = APP_STATUS_OK;

    return APP_STATUS_OK;
}

app_status_t lcd_display_service_process (uint64_t now_us)
{
    app_status_t status = APP_STATUS_BUSY;

    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    switch (g_diagnostics.state)
    {
        case LCD_DISPLAY_STATE_COLOR_BARS:
            if (now_us >= g_stage_deadline_us)
            {
                status = st7796s_controller_fill_rect(0U,
                                                       0U,
                                                       ST7796S_LOGICAL_WIDTH,
                                                       ST7796S_LOGICAL_HEIGHT,
                                                       0x0000U);
                if (APP_STATUS_OK == status)
                {
                    g_diagnostics.state = LCD_DISPLAY_STATE_WAIT_STATIC_FRAME;
                }
            }
            break;

        case LCD_DISPLAY_STATE_WAIT_STATIC_FRAME:
            status = lcd_display_acquire(LCD_DISPLAY_STATE_TRANSFER_STATIC_FRAME);
            if (APP_STATUS_OK == status)
            {
                status = lcd_display_transfer_slice(now_us, true);
            }
            break;

        case LCD_DISPLAY_STATE_TRANSFER_STATIC_FRAME:
            status = lcd_display_transfer_slice(now_us, true);
            break;

        case LCD_DISPLAY_STATE_STATIC_HOLD:
            if (now_us >= g_stage_deadline_us)
            {
                g_diagnostics.state = LCD_DISPLAY_STATE_WAIT_STREAM_FRAME;
                status = APP_STATUS_OK;
            }
            break;

        case LCD_DISPLAY_STATE_WAIT_STREAM_FRAME:
            status = lcd_display_acquire(LCD_DISPLAY_STATE_TRANSFER_STREAM_FRAME);
            if (APP_STATUS_OK == status)
            {
                status = lcd_display_transfer_slice(now_us, false);
            }
            break;

        case LCD_DISPLAY_STATE_TRANSFER_STREAM_FRAME:
            status = lcd_display_transfer_slice(now_us, false);
            break;

        case LCD_DISPLAY_STATE_TRANSFER_HELD_FRAME:
            status = lcd_display_transfer_slice(now_us, false);
            break;

        case LCD_DISPLAY_STATE_HELD_FRAME:
            break;

        case LCD_DISPLAY_STATE_ERROR:
            return g_diagnostics.last_status;

        case LCD_DISPLAY_STATE_CLOSED:
        default:
            return APP_STATUS_NOT_INITIALIZED;
    }

    if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
    {
        return lcd_display_fail(status, now_us);
    }

    g_diagnostics.last_status = status;
    return status;
}

bool lcd_display_service_ready_for_hold (void)
{
    return g_diagnostics.initialized && !g_diagnostics.lease_active &&
           (LCD_DISPLAY_STATE_WAIT_STREAM_FRAME == g_diagnostics.state);
}

app_status_t lcd_display_service_begin_hold (camera_frame_lease_t const * p_lease)
{
    app_status_t status;

    if ((NULL == p_lease) || !lcd_display_service_ready_for_hold())
    {
        return (NULL == p_lease) ? APP_STATUS_INVALID_ARGUMENT : APP_STATUS_BUSY;
    }

    status = camera_frame_pool_validate_lease(p_lease);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    return lcd_display_service_begin_frame_hold(p_lease->p_frame);
}

app_status_t lcd_display_service_begin_frame_hold (camera_frame_t const * p_frame)
{
    app_status_t status;

    if ((NULL == p_frame) || !lcd_display_service_ready_for_hold())
    {
        return (NULL == p_frame) ? APP_STATUS_INVALID_ARGUMENT : APP_STATUS_BUSY;
    }

    status = lcd_display_validate_frame(p_frame);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    g_lease = (camera_frame_lease_t)
    {
        .slot       = CAMERA_FRAME_POOL_NO_SLOT,
        .generation = p_frame->generation,
        .p_frame    = p_frame,
    };
    g_diagnostics.lease_active = true;
    g_diagnostics.external_lease = true;
    g_diagnostics.external_holds_started++;
    g_diagnostics.current_row = 0U;
    g_frame_start_cycles = platform_time_raw_cycles();
    g_diagnostics.state = LCD_DISPLAY_STATE_TRANSFER_HELD_FRAME;
    g_diagnostics.last_status = APP_STATUS_OK;
    return APP_STATUS_OK;
}

bool lcd_display_service_hold_complete (void)
{
    return g_diagnostics.initialized && g_diagnostics.external_lease &&
           g_diagnostics.lease_active &&
           (LCD_DISPLAY_STATE_HELD_FRAME == g_diagnostics.state);
}

app_status_t lcd_display_service_resume_live (void)
{
    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    if (!g_diagnostics.lease_active && !g_diagnostics.external_lease &&
        (LCD_DISPLAY_STATE_WAIT_STREAM_FRAME == g_diagnostics.state))
    {
        return APP_STATUS_OK;
    }

    if (!lcd_display_service_hold_complete())
    {
        return APP_STATUS_BUSY;
    }

    g_lease = (camera_frame_lease_t) {0};
    g_diagnostics.lease_active = false;
    g_diagnostics.external_lease = false;
    g_diagnostics.current_row = 0U;
    g_diagnostics.state = LCD_DISPLAY_STATE_WAIT_STREAM_FRAME;
    g_diagnostics.last_status = APP_STATUS_OK;
    g_diagnostics.external_hold_resumes++;
    return APP_STATUS_OK;
}

app_status_t lcd_display_service_close (void)
{
    app_status_t status = APP_STATUS_OK;

    if (g_diagnostics.lease_active && !g_diagnostics.external_lease)
    {
        status = camera_capture_release(&g_lease);
    }
    g_diagnostics.lease_active = false;
    g_diagnostics.external_lease = false;

    {
        app_status_t const close_status = st7796s_controller_close();

        if (APP_STATUS_OK == status)
        {
            status = close_status;
        }
    }
    g_diagnostics.initialized = false;
    g_diagnostics.state = LCD_DISPLAY_STATE_CLOSED;
    g_diagnostics.last_status = status;

    return status;
}

void lcd_display_service_get_diagnostics (lcd_display_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        *p_diagnostics = g_diagnostics;
    }
}

char const * lcd_display_service_state_name (lcd_display_state_t state)
{
    static char const * const names[] =
    {
        "closed",
        "color-bars",
        "wait-static",
        "transfer-static",
        "static-hold",
        "wait-stream",
        "transfer-stream",
        "transfer-held",
        "held-frame",
        "error",
    };

    if ((uint32_t) state >= (uint32_t) (sizeof(names) / sizeof(names[0])))
    {
        return "unknown";
    }

    return names[state];
}

static app_status_t lcd_display_draw_color_bars (void)
{
    static uint16_t const colors[] =
    {
        0xFFFFU, 0xFFE0U, 0x07FFU, 0x07E0U,
        0xF81FU, 0xF800U, 0x001FU, 0x0000U,
    };
    uint16_t const bar_width = (uint16_t) (ST7796S_LOGICAL_WIDTH /
                                           (uint16_t) (sizeof(colors) / sizeof(colors[0])));

    for (uint16_t index = 0U;
         index < (uint16_t) (sizeof(colors) / sizeof(colors[0]));
         index++)
    {
        app_status_t const status = st7796s_controller_fill_rect((uint16_t) (index * bar_width),
                                                                 0U,
                                                                 bar_width,
                                                                 ST7796S_LOGICAL_HEIGHT,
                                                                 colors[index]);
        if (APP_STATUS_OK != status)
        {
            return status;
        }
    }

    return APP_STATUS_OK;
}

static app_status_t lcd_display_acquire (lcd_display_state_t transfer_state)
{
    app_status_t status = camera_capture_acquire_latest(&g_lease);

    if (APP_STATUS_OK != status)
    {
        if (APP_STATUS_BUSY == status)
        {
            g_diagnostics.acquire_busy++;
        }
        return status;
    }

    g_diagnostics.lease_active = true;
    status = lcd_display_validate_frame(g_lease.p_frame);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    if ((0U != g_diagnostics.last_sequence) &&
        ((int32_t) (g_lease.p_frame->sequence - g_diagnostics.last_sequence) > 1))
    {
        g_diagnostics.skipped_frames += g_lease.p_frame->sequence -
                                        g_diagnostics.last_sequence - 1U;
    }

    g_diagnostics.current_row  = 0U;
    g_frame_start_cycles       = platform_time_raw_cycles();
    g_diagnostics.state        = transfer_state;

    return APP_STATUS_OK;
}

static app_status_t lcd_display_transfer_slice (uint64_t now_us, bool static_frame)
{
    camera_frame_t const * p_frame = g_lease.p_frame;
    uint32_t const slice_start_cycles = platform_time_raw_cycles();
    uint16_t const remaining_rows = (uint16_t) (LCD_PREVIEW_IMAGE_HEIGHT -
                                                g_diagnostics.current_row);
    uint16_t const rows = (remaining_rows > LCD_PREVIEW_ROWS_PER_SLICE) ?
                          LCD_PREVIEW_ROWS_PER_SLICE : remaining_rows;
    app_status_t status = lcd_preview_scaler_scale_slice_rgb565_be(
        (uint8_t const *) p_frame->address,
        p_frame->valid_bytes,
        p_frame->width,
        p_frame->height,
        p_frame->stride,
        g_diagnostics.current_row,
        rows,
        g_preview_slice,
        LCD_PREVIEW_SLICE_PIXELS);

    if (APP_STATUS_OK == status)
    {
        status = st7796s_controller_write_rect(LCD_PREVIEW_X_OFFSET,
                                               (uint16_t) (LCD_PREVIEW_Y_OFFSET +
                                                           g_diagnostics.current_row),
                                               LCD_PREVIEW_IMAGE_WIDTH,
                                               rows,
                                               g_preview_slice);
    }

    if (APP_STATUS_OK != status)
    {
        return status;
    }

    {
        uint32_t slice_us = 0U;

        if (APP_STATUS_OK == platform_time_elapsed_us(slice_start_cycles,
                                                       platform_time_raw_cycles(),
                                                       &slice_us))
        {
            g_diagnostics.last_slice_us = slice_us;
            if (slice_us > g_diagnostics.max_slice_us)
            {
                g_diagnostics.max_slice_us = slice_us;
            }
        }
    }

    g_diagnostics.current_row = (uint16_t) (g_diagnostics.current_row + rows);
    g_diagnostics.transfer_slices++;
    if (g_diagnostics.current_row >= LCD_PREVIEW_IMAGE_HEIGHT)
    {
        uint32_t frame_us = 0U;

        g_diagnostics.last_sequence = p_frame->sequence;
        g_diagnostics.last_crc32    = p_frame->crc32;
        if (g_diagnostics.external_lease)
        {
            g_diagnostics.state = LCD_DISPLAY_STATE_HELD_FRAME;
            g_diagnostics.external_holds_completed++;
        }
        else
        {
            status = camera_capture_release(&g_lease);
            g_diagnostics.lease_active = false;
            if (APP_STATUS_OK != status)
            {
                return status;
            }
        }

        if (APP_STATUS_OK == platform_time_elapsed_us(g_frame_start_cycles,
                                                       platform_time_raw_cycles(),
                                                       &frame_us))
        {
            g_diagnostics.last_frame_us = frame_us;
            if (frame_us > g_diagnostics.max_frame_us)
            {
                g_diagnostics.max_frame_us = frame_us;
            }
        }

        g_diagnostics.completed_frames++;
        if (g_diagnostics.external_lease)
        {
            g_diagnostics.state = LCD_DISPLAY_STATE_HELD_FRAME;
        }
        else if (static_frame)
        {
            g_stage_deadline_us = now_us + LCD_STATIC_FRAME_HOLD_US;
            g_diagnostics.state = LCD_DISPLAY_STATE_STATIC_HOLD;
        }
        else
        {
            g_diagnostics.state = LCD_DISPLAY_STATE_WAIT_STREAM_FRAME;
        }
    }

    return APP_STATUS_OK;
}

static app_status_t lcd_display_validate_frame (camera_frame_t const * p_frame)
{
    if ((NULL == p_frame) || (APP_STATUS_OK != p_frame->status) ||
        (CAMERA_SENSOR_VGA_WIDTH != p_frame->width) ||
        (CAMERA_SENSOR_VGA_HEIGHT != p_frame->height) ||
        (CAMERA_FRAME_STRIDE != p_frame->stride) ||
        ((uint8_t) CAMERA_PIXEL_FORMAT_RGB565_BE != p_frame->pixel_format) ||
        (CAMERA_FRAME_BYTES != p_frame->valid_bytes) ||
        (p_frame->capacity < CAMERA_FRAME_BYTES) ||
        ((uintptr_t) 0U == p_frame->address))
    {
        return APP_STATUS_DATA;
    }

    return APP_STATUS_OK;
}

static app_status_t lcd_display_fail (app_status_t status, uint64_t now_us)
{
    (void) now_us;
    if (g_diagnostics.external_lease)
    {
        g_diagnostics.external_hold_failures++;
    }
    if (g_diagnostics.lease_active && !g_diagnostics.external_lease)
    {
        (void) camera_capture_release(&g_lease);
    }
    g_diagnostics.lease_active = false;
    g_diagnostics.external_lease = false;

    (void) st7796s_controller_backlight_set(false);
    g_diagnostics.errors++;
    g_diagnostics.last_status = status;
    g_diagnostics.state = LCD_DISPLAY_STATE_ERROR;

    return status;
}

#ifndef LCD_DISPLAY_SERVICE_H_
#define LCD_DISPLAY_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "camera_frame_pool.h"

typedef enum e_lcd_display_state
{
    LCD_DISPLAY_STATE_CLOSED = 0,
    LCD_DISPLAY_STATE_COLOR_BARS,
    LCD_DISPLAY_STATE_WAIT_STATIC_FRAME,
    LCD_DISPLAY_STATE_TRANSFER_STATIC_FRAME,
    LCD_DISPLAY_STATE_STATIC_HOLD,
    LCD_DISPLAY_STATE_WAIT_STREAM_FRAME,
    LCD_DISPLAY_STATE_TRANSFER_STREAM_FRAME,
    LCD_DISPLAY_STATE_TRANSFER_HELD_FRAME,
    LCD_DISPLAY_STATE_HELD_FRAME,
    LCD_DISPLAY_STATE_ERROR
} lcd_display_state_t;

typedef struct st_lcd_display_diagnostics
{
    uint32_t completed_frames;
    uint32_t skipped_frames;
    uint32_t transfer_slices;
    uint32_t last_sequence;
    uint32_t last_crc32;
    uint32_t last_frame_us;
    uint32_t max_frame_us;
    uint32_t last_slice_us;
    uint32_t max_slice_us;
    uint32_t acquire_busy;
    uint32_t errors;
    uint32_t external_holds_started;
    uint32_t external_holds_completed;
    uint32_t external_hold_resumes;
    uint32_t external_hold_failures;
    uint16_t current_row;
    app_status_t last_status;
    lcd_display_state_t state;
    bool initialized;
    bool lease_active;
    bool external_lease;
} lcd_display_diagnostics_t;

app_status_t lcd_display_service_init(uint64_t now_us);
app_status_t lcd_display_service_process(uint64_t now_us);
bool         lcd_display_service_ready_for_hold(void);
app_status_t lcd_display_service_begin_hold(camera_frame_lease_t const * p_lease);
app_status_t lcd_display_service_begin_frame_hold(camera_frame_t const * p_frame);
bool         lcd_display_service_hold_complete(void);
app_status_t lcd_display_service_resume_live(void);
app_status_t lcd_display_service_close(void);
void         lcd_display_service_get_diagnostics(lcd_display_diagnostics_t * p_diagnostics);
char const * lcd_display_service_state_name(lcd_display_state_t state);

#endif /* LCD_DISPLAY_SERVICE_H_ */

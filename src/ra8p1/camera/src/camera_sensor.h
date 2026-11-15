#ifndef CAMERA_SENSOR_H_
#define CAMERA_SENSOR_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define CAMERA_SENSOR_CHIP_ID             (0x5640U)
#define CAMERA_SENSOR_ID_TEST_CYCLES      (20U)
#define CAMERA_SENSOR_MODE_VERSION        "atk-mc5640-vga-rgb565-25fps-v5"
#define CAMERA_SENSOR_VGA_WIDTH           (640U)
#define CAMERA_SENSOR_VGA_HEIGHT          (480U)
#define CAMERA_SENSOR_VGA_FRAME_RATE      (25U)
#define CAMERA_SENSOR_VGA_FRAME_PERIOD_US (40000U)
#define CAMERA_SENSOR_PLL_CONTROL0        (0x1AU)
#define CAMERA_SENSOR_PLL_CONTROL1        (0x21U)
#define CAMERA_SENSOR_PLL_CONTROL2        (0x69U)
#define CAMERA_SENSOR_PLL_CONTROL3        (0x13U)
#define CAMERA_SENSOR_SYSTEM_ROOT_DIVIDER (0x01U)
#define CAMERA_SENSOR_VGA_HTS             (1528U)
#define CAMERA_SENSOR_VGA_VTS             (1100U)

typedef enum e_camera_pixel_format
{
    /* OV5640 DVP emits the most-significant RGB565 byte first. */
    CAMERA_PIXEL_FORMAT_RGB565_BE = 0
} camera_pixel_format_t;

typedef struct st_camera_mode
{
    uint16_t              width;
    uint16_t              height;
    camera_pixel_format_t pixel_format;
    uint8_t               frame_rate;
} camera_mode_t;

typedef enum e_camera_sensor_stage
{
    CAMERA_SENSOR_STAGE_CLOSED = 0,
    CAMERA_SENSOR_STAGE_SAFE,
    CAMERA_SENSOR_STAGE_PWDN_RELEASED,
    CAMERA_SENSOR_STAGE_RESET_RELEASED,
    CAMERA_SENSOR_STAGE_ID_HIGH,
    CAMERA_SENSOR_STAGE_ID_LOW,
    CAMERA_SENSOR_STAGE_READY,
    CAMERA_SENSOR_STAGE_SOFTWARE_RESET,
    CAMERA_SENSOR_STAGE_MODE_TABLE,
    CAMERA_SENSOR_STAGE_MODE_READBACK,
    CAMERA_SENSOR_STAGE_CONFIGURED,
    CAMERA_SENSOR_STAGE_STREAMING,
    CAMERA_SENSOR_STAGE_STOPPED,
    CAMERA_SENSOR_STAGE_ERROR
} camera_sensor_stage_t;

typedef struct st_camera_sensor_test_result
{
    uint32_t     cycles_requested;
    uint32_t     cycles_completed;
    uint32_t     cycles_passed;
    uint16_t     chip_id;
    app_status_t status;
} camera_sensor_test_result_t;

typedef struct st_camera_sensor_diagnostics
{
    uint32_t              reset_sequences;
    uint32_t              id_reads;
    uint32_t              id_matches;
    uint32_t              id_mismatches;
    uint32_t              gpio_errors;
    uint32_t              table_entries_written;
    uint32_t              table_entry_count;
    uint32_t              readback_checks;
    uint32_t              readback_mismatches;
    uint32_t              table_index;
    uint32_t              nominal_frame_period_us;
    uint16_t              last_chip_id;
    uint16_t              last_register;
    uint16_t              active_width;
    uint16_t              active_height;
    uint16_t              horizontal_total;
    uint16_t              vertical_total;
    uint8_t               expected_value;
    uint8_t               actual_value;
    uint8_t               pll_control0;
    uint8_t               pll_control1;
    uint8_t               pll_control2;
    uint8_t               pll_control3;
    uint8_t               system_root_divider;
    uint8_t               format_control;
    uint8_t               format_mux;
    uint8_t               stream_control;
    app_status_t          last_status;
    camera_sensor_stage_t stage;
    bool                  initialized;
    bool                  mode_applied;
    bool                  streaming;
} camera_sensor_diagnostics_t;

app_status_t camera_sensor_open(void);
app_status_t camera_sensor_read_id(uint16_t * p_chip_id);
app_status_t camera_sensor_id_self_test(uint32_t cycles, camera_sensor_test_result_t * p_result);
app_status_t camera_sensor_apply_mode(camera_mode_t const * p_mode);
app_status_t camera_sensor_start(void);
app_status_t camera_sensor_stop(void);
void         camera_sensor_get_diagnostics(camera_sensor_diagnostics_t * p_diagnostics);

#endif /* CAMERA_SENSOR_H_ */

#include "camera_sensor.h"

#include <stddef.h>

#include "hal_data.h"
#include "sccb_bus.h"

#define CAMERA_ID_HIGH_REG                 (0x300AU)
#define CAMERA_ID_LOW_REG                  (0x300BU)
#define CAMERA_SYSTEM_CONTROL_REG          (0x3008U)
#define CAMERA_CLOCK_SELECT_REG            (0x3103U)
#define CAMERA_SYSTEM_SOFTWARE_RESET       (0x82U)
#define CAMERA_SYSTEM_STREAM_STOPPED       (0x42U)
#define CAMERA_SYSTEM_STREAMING            (0x02U)
#define CAMERA_RESET_HOLD_MS               (20U)
#define CAMERA_PWDN_RELEASE_MS             (5U)
#define CAMERA_RESET_RELEASE_MS            (20U)
#define CAMERA_SOFTWARE_RESET_MS           (10U)
#define CAMERA_MODE_SETTLE_MS              (5U)

#define CAMERA_ARRAY_SIZE(array)           ((uint32_t) (sizeof(array) / sizeof((array)[0])))

typedef struct st_camera_register_setting
{
    uint16_t reg;
    uint8_t  value;
} camera_register_setting_t;

/*
 * ATK-MC5640 V1.0 common DVP baseline. Reset and stream control register
 * writes are intentionally outside the table so their delays and state
 * transitions remain visible.
 */
static camera_register_setting_t const g_atk_common_registers[] =
{
    {0x3103U, 0x03U},
    {0x3017U, 0xFFU},
    {0x3018U, 0xFFU},
    {0x3034U, CAMERA_SENSOR_PLL_CONTROL0},
    {0x3037U, CAMERA_SENSOR_PLL_CONTROL3},
    {0x3108U, CAMERA_SENSOR_SYSTEM_ROOT_DIVIDER},
    {0x3630U, 0x36U},
    {0x3631U, 0x0EU},
    {0x3632U, 0xE2U},
    {0x3633U, 0x12U},
    {0x3621U, 0xE0U},
    {0x3704U, 0xA0U},
    {0x3703U, 0x5AU},
    {0x3715U, 0x78U},
    {0x3717U, 0x01U},
    {0x370BU, 0x60U},
    {0x3705U, 0x1AU},
    {0x3905U, 0x02U},
    {0x3906U, 0x10U},
    {0x3901U, 0x0AU},
    {0x3731U, 0x12U},
    {0x3600U, 0x08U},
    {0x3601U, 0x33U},
    {0x302DU, 0x60U},
    {0x3620U, 0x52U},
    {0x371BU, 0x20U},
    {0x471CU, 0x50U},
    {0x3A13U, 0x43U},
    {0x3A18U, 0x00U},
    {0x3A19U, 0xF8U},
    {0x3635U, 0x13U},
    {0x3636U, 0x03U},
    {0x3634U, 0x40U},
    {0x3622U, 0x01U},
    {0x3C01U, 0x34U},
    {0x3C04U, 0x28U},
    {0x3C05U, 0x98U},
    {0x3C06U, 0x00U},
    {0x3C07U, 0x08U},
    {0x3C08U, 0x00U},
    {0x3C09U, 0x1CU},
    {0x3C0AU, 0x9CU},
    {0x3C0BU, 0x40U},
    {0x3810U, 0x00U},
    {0x3811U, 0x10U},
    {0x3812U, 0x00U},
    {0x3708U, 0x64U},
    {0x4001U, 0x02U},
    {0x4005U, 0x1AU},
    {0x3000U, 0x00U},
    {0x3004U, 0xFFU},
    {0x300EU, 0x58U},
    {0x302EU, 0x00U},
    {0x4300U, 0x30U},
    {0x501FU, 0x00U},
    {0x440EU, 0x00U},
    {0x5000U, 0xA7U},
    {0x3A0FU, 0x30U},
    {0x3A10U, 0x28U},
    {0x3A1BU, 0x30U},
    {0x3A1EU, 0x26U},
    {0x3A11U, 0x60U},
    {0x3A1FU, 0x14U},
    {0x5800U, 0x23U},
    {0x5801U, 0x14U},
    {0x5802U, 0x0FU},
    {0x5803U, 0x0FU},
    {0x5804U, 0x12U},
    {0x5805U, 0x26U},
    {0x5806U, 0x0CU},
    {0x5807U, 0x08U},
    {0x5808U, 0x05U},
    {0x5809U, 0x05U},
    {0x580AU, 0x08U},
    {0x580BU, 0x0DU},
    {0x580CU, 0x08U},
    {0x580DU, 0x03U},
    {0x580EU, 0x00U},
    {0x580FU, 0x00U},
    {0x5810U, 0x03U},
    {0x5811U, 0x09U},
    {0x5812U, 0x07U},
    {0x5813U, 0x03U},
    {0x5814U, 0x00U},
    {0x5815U, 0x01U},
    {0x5816U, 0x03U},
    {0x5817U, 0x08U},
    {0x5818U, 0x0DU},
    {0x5819U, 0x08U},
    {0x581AU, 0x05U},
    {0x581BU, 0x06U},
    {0x581CU, 0x08U},
    {0x581DU, 0x0EU},
    {0x581EU, 0x29U},
    {0x581FU, 0x17U},
    {0x5820U, 0x11U},
    {0x5821U, 0x11U},
    {0x5822U, 0x15U},
    {0x5823U, 0x28U},
    {0x5824U, 0x46U},
    {0x5825U, 0x26U},
    {0x5826U, 0x08U},
    {0x5827U, 0x26U},
    {0x5828U, 0x64U},
    {0x5829U, 0x26U},
    {0x582AU, 0x24U},
    {0x582BU, 0x22U},
    {0x582CU, 0x24U},
    {0x582DU, 0x24U},
    {0x582EU, 0x06U},
    {0x582FU, 0x22U},
    {0x5830U, 0x40U},
    {0x5831U, 0x42U},
    {0x5832U, 0x24U},
    {0x5833U, 0x26U},
    {0x5834U, 0x24U},
    {0x5835U, 0x22U},
    {0x5836U, 0x22U},
    {0x5837U, 0x26U},
    {0x5838U, 0x44U},
    {0x5839U, 0x24U},
    {0x583AU, 0x26U},
    {0x583BU, 0x28U},
    {0x583CU, 0x42U},
    {0x583DU, 0xCEU},
    {0x5180U, 0xFFU},
    {0x5181U, 0xF2U},
    {0x5182U, 0x00U},
    {0x5183U, 0x14U},
    {0x5184U, 0x25U},
    {0x5185U, 0x24U},
    {0x5186U, 0x09U},
    {0x5187U, 0x09U},
    {0x5188U, 0x09U},
    {0x5189U, 0x75U},
    {0x518AU, 0x54U},
    {0x518BU, 0xE0U},
    {0x518CU, 0xB2U},
    {0x518DU, 0x42U},
    {0x518EU, 0x3DU},
    {0x518FU, 0x56U},
    {0x5190U, 0x46U},
    {0x5191U, 0xF8U},
    {0x5192U, 0x04U},
    {0x5193U, 0x70U},
    {0x5194U, 0xF0U},
    {0x5195U, 0xF0U},
    {0x5196U, 0x03U},
    {0x5197U, 0x01U},
    {0x5198U, 0x04U},
    {0x5199U, 0x12U},
    {0x519AU, 0x04U},
    {0x519BU, 0x00U},
    {0x519CU, 0x06U},
    {0x519DU, 0x82U},
    {0x519EU, 0x38U},
    {0x5480U, 0x01U},
    {0x5481U, 0x08U},
    {0x5482U, 0x14U},
    {0x5483U, 0x28U},
    {0x5484U, 0x51U},
    {0x5485U, 0x65U},
    {0x5486U, 0x71U},
    {0x5487U, 0x7DU},
    {0x5488U, 0x87U},
    {0x5489U, 0x91U},
    {0x548AU, 0x9AU},
    {0x548BU, 0xAAU},
    {0x548CU, 0xB8U},
    {0x548DU, 0xCDU},
    {0x548EU, 0xDDU},
    {0x548FU, 0xEAU},
    {0x5490U, 0x1DU},
    {0x5381U, 0x1EU},
    {0x5382U, 0x5BU},
    {0x5383U, 0x08U},
    {0x5384U, 0x0AU},
    {0x5385U, 0x7EU},
    {0x5386U, 0x88U},
    {0x5387U, 0x7CU},
    {0x5388U, 0x6CU},
    {0x5389U, 0x10U},
    {0x538AU, 0x01U},
    {0x538BU, 0x98U},
    {0x5580U, 0x06U},
    {0x5583U, 0x40U},
    {0x5584U, 0x10U},
    {0x5589U, 0x10U},
    {0x558AU, 0x00U},
    {0x558BU, 0xF8U},
    {0x501DU, 0x40U},
    {0x5300U, 0x08U},
    {0x5301U, 0x30U},
    {0x5302U, 0x10U},
    {0x5303U, 0x00U},
    {0x5304U, 0x08U},
    {0x5305U, 0x30U},
    {0x5306U, 0x08U},
    {0x5307U, 0x16U},
    {0x5309U, 0x08U},
    {0x530AU, 0x30U},
    {0x530BU, 0x04U},
    {0x530CU, 0x06U},
    {0x5025U, 0x00U},
    {0x4740U, 0x21U},
};

/*
 * ATK VGA RGB565 timing. Runtime telemetry uses an asynchronous UART queue so
 * CEU servicing is not delayed by wire-speed debug output.
 */
static camera_register_setting_t const g_vga_rgb565_registers[] =
{
    {0x4300U, 0x6FU},
    {0x501FU, 0x01U},
    {0x3035U, CAMERA_SENSOR_PLL_CONTROL1},
    {0x3036U, CAMERA_SENSOR_PLL_CONTROL2},
    {0x3C07U, 0x07U},
    {0x3820U, 0x46U},
    {0x3821U, 0x00U},
    {0x3814U, 0x31U},
    {0x3815U, 0x31U},
    {0x3800U, 0x00U},
    {0x3801U, 0x00U},
    {0x3802U, 0x00U},
    {0x3803U, 0x00U},
    {0x3804U, 0x0AU},
    {0x3805U, 0x3FU},
    {0x3806U, 0x06U},
    {0x3807U, 0xA9U},
    {0x3808U, 0x02U},
    {0x3809U, 0x80U},
    {0x380AU, 0x01U},
    {0x380BU, 0xE0U},
    {0x380CU, (uint8_t) (CAMERA_SENSOR_VGA_HTS >> 8U)},
    {0x380DU, (uint8_t) CAMERA_SENSOR_VGA_HTS},
    {0x380EU, (uint8_t) (CAMERA_SENSOR_VGA_VTS >> 8U)},
    {0x380FU, (uint8_t) CAMERA_SENSOR_VGA_VTS},
    {0x3813U, 0x04U},
    {0x3618U, 0x00U},
    {0x3612U, 0x29U},
    {0x3709U, 0x52U},
    {0x370CU, 0x03U},
    {0x3A02U, 0x02U},
    {0x3A03U, 0xE0U},
    {0x3A14U, 0x02U},
    {0x3A15U, 0xE0U},
    {0x4004U, 0x02U},
    {0x3002U, 0x1CU},
    {0x3006U, 0xC3U},
    {0x4713U, 0x03U},
    {0x4407U, 0x04U},
    {0x460BU, 0x37U},
    {0x460CU, 0x20U},
    {0x4837U, 0x16U},
    {0x3824U, 0x04U},
    {0x5001U, 0xA3U},
    {0x3503U, 0x00U},
};

static camera_register_setting_t const g_mode_readback[] =
{
    {0x3034U, CAMERA_SENSOR_PLL_CONTROL0},
    {0x3035U, CAMERA_SENSOR_PLL_CONTROL1},
    {0x3036U, CAMERA_SENSOR_PLL_CONTROL2},
    {0x3037U, CAMERA_SENSOR_PLL_CONTROL3},
    {0x3108U, CAMERA_SENSOR_SYSTEM_ROOT_DIVIDER},
    {0x3814U, 0x31U},
    {0x3815U, 0x31U},
    {0x380CU, (uint8_t) (CAMERA_SENSOR_VGA_HTS >> 8U)},
    {0x380DU, (uint8_t) CAMERA_SENSOR_VGA_HTS},
    {0x380EU, (uint8_t) (CAMERA_SENSOR_VGA_VTS >> 8U)},
    {0x380FU, (uint8_t) CAMERA_SENSOR_VGA_VTS},
    {0x3820U, 0x46U},
    {0x3821U, 0x00U},
    {0x3808U, 0x02U},
    {0x3809U, 0x80U},
    {0x380AU, 0x01U},
    {0x380BU, 0xE0U},
    {0x4300U, 0x6FU},
    {0x501FU, 0x01U},
    {0x4740U, 0x21U},
    {CAMERA_SYSTEM_CONTROL_REG, CAMERA_SYSTEM_STREAM_STOPPED},
};

static camera_sensor_diagnostics_t g_diagnostics;

static app_status_t camera_sensor_reset_sequence(void);
static app_status_t camera_sensor_software_reset(void);
static app_status_t camera_sensor_apply_table(camera_register_setting_t const * p_table,
                                              uint32_t table_count,
                                              uint32_t index_offset);
static app_status_t camera_sensor_verify_table(camera_register_setting_t const * p_table, uint32_t table_count);
static app_status_t camera_sensor_write_register(uint16_t reg, uint8_t value);
static app_status_t camera_sensor_verify_register(uint16_t reg, uint8_t expected);
static app_status_t camera_sensor_write_pin(bsp_io_port_pin_t pin, bsp_io_level_t level);
static app_status_t camera_sensor_record_status(app_status_t status);

app_status_t camera_sensor_open (void)
{
    app_status_t status;

    g_diagnostics = (camera_sensor_diagnostics_t) {0};

    status = camera_sensor_write_pin(CAMERA_RESET, BSP_IO_LEVEL_LOW);
    if (APP_STATUS_OK == status)
    {
        status = camera_sensor_write_pin(CAMERA_PWDN, BSP_IO_LEVEL_HIGH);
    }

    if (APP_STATUS_OK != status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    g_diagnostics.initialized = true;
    g_diagnostics.stage       = CAMERA_SENSOR_STAGE_SAFE;

    return camera_sensor_record_status(APP_STATUS_OK);
}

app_status_t camera_sensor_read_id (uint16_t * p_chip_id)
{
    app_status_t status;
    uint8_t      id_high = 0U;
    uint8_t      id_low = 0U;
    uint16_t     chip_id;

    if (!g_diagnostics.initialized)
    {
        return camera_sensor_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    if (NULL == p_chip_id)
    {
        return camera_sensor_record_status(APP_STATUS_INVALID_ARGUMENT);
    }

    g_diagnostics.stage = CAMERA_SENSOR_STAGE_ID_HIGH;
    status = sccb_read_u8(CAMERA_ID_HIGH_REG, &id_high, SCCB_BUS_MAX_TIMEOUT_US);
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.last_register = CAMERA_ID_HIGH_REG;
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    g_diagnostics.stage = CAMERA_SENSOR_STAGE_ID_LOW;
    status = sccb_read_u8(CAMERA_ID_LOW_REG, &id_low, SCCB_BUS_MAX_TIMEOUT_US);
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.last_register = CAMERA_ID_LOW_REG;
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    chip_id = (uint16_t) (((uint16_t) id_high << 8U) | id_low);
    *p_chip_id = chip_id;
    g_diagnostics.id_reads++;
    g_diagnostics.last_chip_id = chip_id;

    if (CAMERA_SENSOR_CHIP_ID != chip_id)
    {
        g_diagnostics.id_mismatches++;
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(APP_STATUS_DATA);
    }

    g_diagnostics.id_matches++;
    g_diagnostics.stage = CAMERA_SENSOR_STAGE_READY;

    return camera_sensor_record_status(APP_STATUS_OK);
}

app_status_t camera_sensor_id_self_test (uint32_t cycles, camera_sensor_test_result_t * p_result)
{
    app_status_t status = APP_STATUS_OK;

    if (!g_diagnostics.initialized)
    {
        return camera_sensor_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    if ((0U == cycles) || (NULL == p_result))
    {
        return camera_sensor_record_status(APP_STATUS_INVALID_ARGUMENT);
    }

    *p_result = (camera_sensor_test_result_t)
    {
        .cycles_requested = cycles,
        .status = APP_STATUS_OK,
    };

    for (uint32_t cycle = 0U; cycle < cycles; cycle++)
    {
        status = camera_sensor_reset_sequence();
        if (APP_STATUS_OK != status)
        {
            break;
        }

        status = camera_sensor_read_id(&p_result->chip_id);
        if ((APP_STATUS_OK == status) || (APP_STATUS_DATA == status))
        {
            p_result->cycles_completed++;
        }

        if (APP_STATUS_OK != status)
        {
            break;
        }

        p_result->cycles_passed++;
    }

    p_result->status = status;
    if ((APP_STATUS_OK == status) && (p_result->cycles_passed != cycles))
    {
        p_result->status = APP_STATUS_DATA;
    }

    if (APP_STATUS_OK != p_result->status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
    }

    return camera_sensor_record_status(p_result->status);
}

app_status_t camera_sensor_apply_mode (camera_mode_t const * p_mode)
{
    app_status_t status;
    uint32_t const common_count = CAMERA_ARRAY_SIZE(g_atk_common_registers);
    uint32_t const mode_count = CAMERA_ARRAY_SIZE(g_vga_rgb565_registers);

    if (!g_diagnostics.initialized)
    {
        return camera_sensor_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    if (NULL == p_mode)
    {
        return camera_sensor_record_status(APP_STATUS_INVALID_ARGUMENT);
    }

    if ((CAMERA_SENSOR_VGA_WIDTH != p_mode->width) ||
        (CAMERA_SENSOR_VGA_HEIGHT != p_mode->height) ||
        (CAMERA_PIXEL_FORMAT_RGB565_BE != p_mode->pixel_format) ||
        (CAMERA_SENSOR_VGA_FRAME_RATE != p_mode->frame_rate))
    {
        return camera_sensor_record_status(APP_STATUS_INVALID_ARGUMENT);
    }

    g_diagnostics.mode_applied          = false;
    g_diagnostics.streaming             = false;
    g_diagnostics.table_entries_written = 0U;
    g_diagnostics.table_entry_count     = common_count + mode_count;
    g_diagnostics.table_index           = 0U;
    g_diagnostics.nominal_frame_period_us = CAMERA_SENSOR_VGA_FRAME_PERIOD_US;
    g_diagnostics.active_width          = 0U;
    g_diagnostics.active_height         = 0U;
    g_diagnostics.horizontal_total      = 0U;
    g_diagnostics.vertical_total        = 0U;
    g_diagnostics.pll_control0          = 0U;
    g_diagnostics.pll_control1          = 0U;
    g_diagnostics.pll_control2          = 0U;
    g_diagnostics.pll_control3          = 0U;
    g_diagnostics.system_root_divider   = 0U;
    g_diagnostics.format_control        = 0U;
    g_diagnostics.format_mux            = 0U;
    g_diagnostics.stream_control        = 0U;

    status = camera_sensor_software_reset();
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    status = camera_sensor_apply_table(g_atk_common_registers, common_count, 0U);
    if (APP_STATUS_OK == status)
    {
        status = camera_sensor_apply_table(g_vga_rgb565_registers, mode_count, common_count);
    }

    if (APP_STATUS_OK != status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    R_BSP_SoftwareDelay(CAMERA_MODE_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
    g_diagnostics.stage = CAMERA_SENSOR_STAGE_MODE_READBACK;
    status = camera_sensor_verify_table(g_mode_readback, CAMERA_ARRAY_SIZE(g_mode_readback));
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    g_diagnostics.mode_applied = true;
    g_diagnostics.stage        = CAMERA_SENSOR_STAGE_CONFIGURED;

    return camera_sensor_record_status(APP_STATUS_OK);
}

app_status_t camera_sensor_start (void)
{
    app_status_t status;

    if ((!g_diagnostics.initialized) || (!g_diagnostics.mode_applied))
    {
        return camera_sensor_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    if (g_diagnostics.streaming)
    {
        return camera_sensor_record_status(APP_STATUS_BUSY);
    }

    status = camera_sensor_write_register(CAMERA_SYSTEM_CONTROL_REG, CAMERA_SYSTEM_STREAMING);
    if (APP_STATUS_OK == status)
    {
        R_BSP_SoftwareDelay(CAMERA_MODE_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
        status = camera_sensor_verify_register(CAMERA_SYSTEM_CONTROL_REG, CAMERA_SYSTEM_STREAMING);
    }

    if (APP_STATUS_OK != status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    g_diagnostics.streaming = true;
    g_diagnostics.stage     = CAMERA_SENSOR_STAGE_STREAMING;

    return camera_sensor_record_status(APP_STATUS_OK);
}

app_status_t camera_sensor_stop (void)
{
    app_status_t status;

    if (!g_diagnostics.initialized)
    {
        return camera_sensor_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    status = camera_sensor_write_register(CAMERA_SYSTEM_CONTROL_REG, CAMERA_SYSTEM_STREAM_STOPPED);
    if (APP_STATUS_OK == status)
    {
        R_BSP_SoftwareDelay(CAMERA_MODE_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
        status = camera_sensor_verify_register(CAMERA_SYSTEM_CONTROL_REG, CAMERA_SYSTEM_STREAM_STOPPED);
    }

    if (APP_STATUS_OK != status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    g_diagnostics.streaming = false;
    g_diagnostics.stage     = CAMERA_SENSOR_STAGE_STOPPED;

    return camera_sensor_record_status(APP_STATUS_OK);
}

void camera_sensor_get_diagnostics (camera_sensor_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        *p_diagnostics = g_diagnostics;
    }
}

static app_status_t camera_sensor_reset_sequence (void)
{
    app_status_t status;

    status = camera_sensor_write_pin(CAMERA_RESET, BSP_IO_LEVEL_LOW);
    if (APP_STATUS_OK == status)
    {
        status = camera_sensor_write_pin(CAMERA_PWDN, BSP_IO_LEVEL_HIGH);
    }

    if (APP_STATUS_OK != status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    g_diagnostics.stage = CAMERA_SENSOR_STAGE_SAFE;
    R_BSP_SoftwareDelay(CAMERA_RESET_HOLD_MS, BSP_DELAY_UNITS_MILLISECONDS);

    status = camera_sensor_write_pin(CAMERA_PWDN, BSP_IO_LEVEL_LOW);
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    g_diagnostics.stage = CAMERA_SENSOR_STAGE_PWDN_RELEASED;
    R_BSP_SoftwareDelay(CAMERA_PWDN_RELEASE_MS, BSP_DELAY_UNITS_MILLISECONDS);

    status = camera_sensor_write_pin(CAMERA_RESET, BSP_IO_LEVEL_HIGH);
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.stage = CAMERA_SENSOR_STAGE_ERROR;
        return camera_sensor_record_status(status);
    }

    g_diagnostics.stage = CAMERA_SENSOR_STAGE_RESET_RELEASED;
    R_BSP_SoftwareDelay(CAMERA_RESET_RELEASE_MS, BSP_DELAY_UNITS_MILLISECONDS);
    g_diagnostics.reset_sequences++;
    g_diagnostics.mode_applied = false;
    g_diagnostics.streaming    = false;

    return camera_sensor_record_status(APP_STATUS_OK);
}

static app_status_t camera_sensor_software_reset (void)
{
    app_status_t status;
    uint8_t      clock_select = 0U;

    g_diagnostics.stage = CAMERA_SENSOR_STAGE_SOFTWARE_RESET;
    status = sccb_read_u8(CAMERA_CLOCK_SELECT_REG, &clock_select, SCCB_BUS_MAX_TIMEOUT_US);
    if (APP_STATUS_OK == status)
    {
        status = camera_sensor_write_register(CAMERA_CLOCK_SELECT_REG, (uint8_t) (clock_select & (uint8_t) ~0x02U));
    }

    if (APP_STATUS_OK == status)
    {
        status = camera_sensor_write_register(CAMERA_SYSTEM_CONTROL_REG, CAMERA_SYSTEM_SOFTWARE_RESET);
    }

    if (APP_STATUS_OK != status)
    {
        return status;
    }

    R_BSP_SoftwareDelay(CAMERA_SOFTWARE_RESET_MS, BSP_DELAY_UNITS_MILLISECONDS);
    status = camera_sensor_write_register(CAMERA_SYSTEM_CONTROL_REG, CAMERA_SYSTEM_STREAM_STOPPED);
    if (APP_STATUS_OK == status)
    {
        R_BSP_SoftwareDelay(CAMERA_MODE_SETTLE_MS, BSP_DELAY_UNITS_MILLISECONDS);
    }

    return status;
}

static app_status_t camera_sensor_apply_table (camera_register_setting_t const * p_table,
                                               uint32_t table_count,
                                               uint32_t index_offset)
{
    if ((NULL == p_table) || (0U == table_count))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    g_diagnostics.stage = CAMERA_SENSOR_STAGE_MODE_TABLE;
    for (uint32_t index = 0U; index < table_count; index++)
    {
        app_status_t const status = camera_sensor_write_register(p_table[index].reg, p_table[index].value);

        g_diagnostics.table_index = index_offset + index;
        if (APP_STATUS_OK != status)
        {
            return status;
        }

        g_diagnostics.table_entries_written++;
    }

    return APP_STATUS_OK;
}

static app_status_t camera_sensor_verify_table (camera_register_setting_t const * p_table, uint32_t table_count)
{
    if ((NULL == p_table) || (0U == table_count))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    for (uint32_t index = 0U; index < table_count; index++)
    {
        app_status_t const status = camera_sensor_verify_register(p_table[index].reg, p_table[index].value);

        if (APP_STATUS_OK != status)
        {
            return status;
        }
    }

    return APP_STATUS_OK;
}

static app_status_t camera_sensor_write_register (uint16_t reg, uint8_t value)
{
    g_diagnostics.last_register = reg;
    g_diagnostics.expected_value = value;

    return sccb_write_u8(reg, value, SCCB_BUS_MAX_TIMEOUT_US);
}

static app_status_t camera_sensor_verify_register (uint16_t reg, uint8_t expected)
{
    app_status_t status;
    uint8_t      actual = 0U;

    g_diagnostics.last_register = reg;
    g_diagnostics.expected_value = expected;
    status = sccb_read_u8(reg, &actual, SCCB_BUS_MAX_TIMEOUT_US);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    g_diagnostics.readback_checks++;
    g_diagnostics.actual_value = actual;
    switch (reg)
    {
        case 0x3034U:
            g_diagnostics.pll_control0 = actual;
            break;

        case 0x3035U:
            g_diagnostics.pll_control1 = actual;
            break;

        case 0x3036U:
            g_diagnostics.pll_control2 = actual;
            break;

        case 0x3037U:
            g_diagnostics.pll_control3 = actual;
            break;

        case 0x3108U:
            g_diagnostics.system_root_divider = actual;
            break;

        case 0x380CU:
            g_diagnostics.horizontal_total = (uint16_t) ((uint16_t) actual << 8U);
            break;

        case 0x380DU:
            g_diagnostics.horizontal_total = (uint16_t) (g_diagnostics.horizontal_total | actual);
            break;

        case 0x380EU:
            g_diagnostics.vertical_total = (uint16_t) ((uint16_t) actual << 8U);
            break;

        case 0x380FU:
            g_diagnostics.vertical_total = (uint16_t) (g_diagnostics.vertical_total | actual);
            break;

        case 0x3808U:
            g_diagnostics.active_width = (uint16_t) ((uint16_t) actual << 8U);
            break;

        case 0x3809U:
            g_diagnostics.active_width = (uint16_t) (g_diagnostics.active_width | actual);
            break;

        case 0x380AU:
            g_diagnostics.active_height = (uint16_t) ((uint16_t) actual << 8U);
            break;

        case 0x380BU:
            g_diagnostics.active_height = (uint16_t) (g_diagnostics.active_height | actual);
            break;

        case 0x4300U:
            g_diagnostics.format_control = actual;
            break;

        case 0x501FU:
            g_diagnostics.format_mux = actual;
            break;

        case CAMERA_SYSTEM_CONTROL_REG:
            g_diagnostics.stream_control = actual;
            break;

        default:
            break;
    }
    if (expected != actual)
    {
        g_diagnostics.readback_mismatches++;
        return APP_STATUS_DATA;
    }

    return APP_STATUS_OK;
}

static app_status_t camera_sensor_write_pin (bsp_io_port_pin_t pin, bsp_io_level_t level)
{
    fsp_err_t err = g_ioport.p_api->pinWrite(g_ioport.p_ctrl, pin, level);

    if (FSP_SUCCESS != err)
    {
        g_diagnostics.gpio_errors++;
        return APP_STATUS_IO;
    }

    return APP_STATUS_OK;
}

static app_status_t camera_sensor_record_status (app_status_t status)
{
    g_diagnostics.last_status = status;

    return status;
}

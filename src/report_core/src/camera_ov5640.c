#include "camera_ov5640.h"

#include "app_config.h"
#include "board_port.h"

#define OV5640_SCCB_ADDRESS 0x3CU
#define OV5640_REG_CHIP_ID_HIGH 0x300AU
#define OV5640_REG_CHIP_ID_LOW  0x300BU

typedef struct
{
    uint16_t reg;
    uint8_t value;
    uint16_t delay_ms;
} camera_register_t;

/* Key output settings; the board camera profile owns clock and DVP timing. */
static const camera_register_t g_ov5640_registers[] =
{
    {0x3008U, 0x82U, 5U},  /* software reset */
    {0x3008U, 0x42U, 1U},  /* software power-down while configuring */
    {0x3103U, 0x03U, 0U},  /* system clock from PLL */
    {0x4300U, 0x61U, 0U},  /* RGB565 output */
    {0x501FU, 0x01U, 0U},  /* ISP RGB path */
    {0x3808U, 0x02U, 0U},  /* width high: 640 */
    {0x3809U, 0x80U, 0U},
    {0x380AU, 0x01U, 0U},  /* height high: 480 */
    {0x380BU, 0xE0U, 0U},
    {0x3008U, 0x02U, 5U}   /* leave power-down */
};

static uint8_t rgb565_luma(const uint8_t * pixel)
{
    uint16_t packed = (uint16_t) pixel[0] | ((uint16_t) pixel[1] << 8U);
    uint8_t red = (uint8_t) (((packed >> 11U) & 0x1FU) << 3U);
    uint8_t green = (uint8_t) (((packed >> 5U) & 0x3FU) << 2U);
    uint8_t blue = (uint8_t) ((packed & 0x1FU) << 3U);
    return (uint8_t) ((77U * red + 150U * green + 29U * blue) >> 8U);
}

app_status_t camera_ov5640_init(void)
{
    uint8_t id_high = 0U;
    uint8_t id_low = 0U;
    size_t index;
    app_status_t status;

    status = board_sccb_read(OV5640_SCCB_ADDRESS, OV5640_REG_CHIP_ID_HIGH, &id_high);
    if (status != APP_OK)
    {
        return status;
    }
    status = board_sccb_read(OV5640_SCCB_ADDRESS, OV5640_REG_CHIP_ID_LOW, &id_low);
    if (status != APP_OK)
    {
        return status;
    }
    if ((id_high != 0x56U) || (id_low != 0x40U))
    {
        return APP_ERR_NOT_READY;
    }

    for (index = 0U; index < (sizeof(g_ov5640_registers) / sizeof(g_ov5640_registers[0])); ++index)
    {
        status = board_sccb_write(OV5640_SCCB_ADDRESS,
                                  g_ov5640_registers[index].reg,
                                  g_ov5640_registers[index].value);
        if (status != APP_OK)
        {
            return status;
        }
        if (g_ov5640_registers[index].delay_ms > 0U)
        {
            board_delay_us((uint32_t) g_ov5640_registers[index].delay_ms * 1000U);
        }
    }

    return APP_OK;
}

app_status_t camera_ov5640_capture(image_t * frame, uint32_t timeout_ms)
{
    uint32_t start_ms;
    size_t required;
    app_status_t status;

    if ((frame == NULL) || (frame->data == NULL) || (frame->format != PIXEL_FORMAT_RGB565))
    {
        return APP_ERR_ARGUMENT;
    }

    required = (size_t) frame->stride_bytes * frame->height;
    if ((frame->width != CAMERA_FRAME_WIDTH) ||
        (frame->height != CAMERA_FRAME_HEIGHT) ||
        (frame->stride_bytes < CAMERA_FRAME_STRIDE))
    {
        return APP_ERR_ARGUMENT;
    }

    status = board_camera_start(frame->data, required);
    if (status != APP_OK)
    {
        return status;
    }

    start_ms = board_time_ms();
    while (!board_camera_frame_done())
    {
        if ((uint32_t) (board_time_ms() - start_ms) >= timeout_ms)
        {
            board_camera_abort();
            return APP_ERR_TIMEOUT;
        }
    }

    return APP_OK;
}

app_status_t camera_check_image_quality(const image_t * frame, image_quality_t * quality)
{
    uint64_t luma_sum = 0U;
    uint64_t laplacian_square_sum = 0U;
    uint32_t dark_count = 0U;
    uint32_t bright_count = 0U;
    uint32_t sample_count = 0U;
    uint32_t sharpness_count = 0U;
    uint16_t x;
    uint16_t y;

    if ((frame == NULL) || (quality == NULL) || (frame->data == NULL) ||
        (frame->format != PIXEL_FORMAT_RGB565) || (frame->width < 3U) || (frame->height < 3U))
    {
        return APP_ERR_ARGUMENT;
    }

    for (y = 2U; y < (uint16_t) (frame->height - 2U); y = (uint16_t) (y + 4U))
    {
        const uint8_t * row = frame->data + ((size_t) y * frame->stride_bytes);
        const uint8_t * row_up = row - frame->stride_bytes;
        const uint8_t * row_down = row + frame->stride_bytes;

        for (x = 2U; x < (uint16_t) (frame->width - 2U); x = (uint16_t) (x + 4U))
        {
            uint8_t center = rgb565_luma(row + ((size_t) x * 2U));
            int32_t laplacian = 4 * (int32_t) center;

            luma_sum += center;
            dark_count += (center < 32U) ? 1U : 0U;
            bright_count += (center > 224U) ? 1U : 0U;
            ++sample_count;

            laplacian -= rgb565_luma(row + ((size_t) (x - 1U) * 2U));
            laplacian -= rgb565_luma(row + ((size_t) (x + 1U) * 2U));
            laplacian -= rgb565_luma(row_up + ((size_t) x * 2U));
            laplacian -= rgb565_luma(row_down + ((size_t) x * 2U));
            laplacian_square_sum += (uint64_t) (laplacian * laplacian);
            ++sharpness_count;
        }
    }

    if ((sample_count == 0U) || (sharpness_count == 0U))
    {
        return APP_ERR_BAD_IMAGE;
    }

    quality->mean_luma = (float) luma_sum / (float) sample_count;
    quality->dark_ratio = (float) dark_count / (float) sample_count;
    quality->bright_ratio = (float) bright_count / (float) sample_count;
    quality->sharpness = (float) laplacian_square_sum / (float) sharpness_count;
    quality->code = IMAGE_QUALITY_OK;

    if ((quality->mean_luma < IMAGE_DARK_MEAN_LIMIT) ||
        (quality->dark_ratio > IMAGE_DARK_PIXEL_RATIO_MAX))
    {
        quality->code = IMAGE_QUALITY_TOO_DARK;
    }
    else if ((quality->mean_luma > IMAGE_BRIGHT_MEAN_LIMIT) ||
             (quality->bright_ratio > IMAGE_BRIGHT_PIXEL_RATIO_MAX))
    {
        quality->code = IMAGE_QUALITY_TOO_BRIGHT;
    }
    else if (quality->sharpness < IMAGE_SHARPNESS_MIN)
    {
        quality->code = IMAGE_QUALITY_BLURRED;
    }

    return APP_OK;
}

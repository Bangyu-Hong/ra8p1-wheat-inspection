#include "wheat_model_storage.h"

#include <stddef.h>
#include <string.h>

#include "hal_data.h"
#include "crc32_ieee.h"
#include "ruhmi_model_api.h"

#define WHEAT_MODEL_IMAGE_CRC32        (0x297619A7UL)
#define WHEAT_COMMAND_STREAM_CRC32      (0x1B6F13E6UL)
#define WHEAT_OSPI0_CS0_BASE           (0x80000000UL)
#define WHEAT_OSPI0_CS0_END            (0x82000000UL)
#define WHEAT_OSPI0_CS0_BYTES          (WHEAT_OSPI0_CS0_END - WHEAT_OSPI0_CS0_BASE)
#define WHEAT_SDRAM_BASE               (0x68000000UL)
#define WHEAT_SDRAM_END                (0x69000000UL)

static wheat_model_storage_info_t g_storage_info;
static app_status_t wheat_model_storage_fail(app_status_t status);

app_status_t wheat_model_storage_init (void)
{
    app_status_t model_source_status;
    app_status_t model_runtime_status;
    app_status_t command_source_status;
    app_status_t command_runtime_status;
    fsp_err_t err;
    uintptr_t const source_end = (uintptr_t) g_wheat_model_image + RUHMI_MODEL_DATA_BYTES;
    uintptr_t const runtime_end = (uintptr_t) sub_0004_model_data + RUHMI_MODEL_DATA_BYTES;
    uintptr_t const command_source_end =
        (uintptr_t) g_wheat_command_stream_image + RUHMI_COMMAND_STREAM_BYTES;
    uintptr_t const command_runtime_end =
        (uintptr_t) sub_0004_command_stream + RUHMI_COMMAND_STREAM_BYTES;

    g_storage_info = (wheat_model_storage_info_t)
    {
        .source_address  = (uintptr_t) g_wheat_model_image,
        .runtime_address = (uintptr_t) sub_0004_model_data,
        .bytes           = RUHMI_MODEL_DATA_BYTES,
        .expected_crc32  = WHEAT_MODEL_IMAGE_CRC32,
        .command_stream_source_address = (uintptr_t) g_wheat_command_stream_image,
        .command_stream_runtime_address = (uintptr_t) sub_0004_command_stream,
        .command_stream_bytes = RUHMI_COMMAND_STREAM_BYTES,
        .command_stream_expected_crc32 = WHEAT_COMMAND_STREAM_CRC32,
        .configured_capacity_bytes = WHEAT_OSPI0_CS0_BYTES,
        /* The full-range CRC protects this image.  JEDEC identity is a
         * separate board-qualification gate because the schematic permits
         * mutually exclusive 4 MiB and 32 MiB flash assemblies. */
        .jedec_identity_verified = false,
    };

    if ((RUHMI_MODEL_DATA_BYTES != sub_0004_model_data_size) ||
        (RUHMI_COMMAND_STREAM_BYTES != sub_0004_command_stream_size) ||
        (g_storage_info.source_address < WHEAT_OSPI0_CS0_BASE) ||
        (source_end > WHEAT_OSPI0_CS0_END) ||
        (g_storage_info.command_stream_source_address < source_end) ||
        (command_source_end > WHEAT_OSPI0_CS0_END) ||
        (g_storage_info.runtime_address < WHEAT_SDRAM_BASE) ||
        (runtime_end > WHEAT_SDRAM_END) ||
        (g_storage_info.command_stream_runtime_address < WHEAT_SDRAM_BASE) ||
        (command_runtime_end > WHEAT_SDRAM_END))
    {
        return APP_STATUS_DATA;
    }

    err = g_ospi0.p_api->open(g_ospi0.p_ctrl, g_ospi0.p_cfg);
    g_storage_info.fsp_status = (int32_t) err;
    if (FSP_SUCCESS != err)
    {
        return APP_STATUS_IO;
    }
    g_storage_info.ospi_open = true;

    /* Match the CPKHMI QSPI reference sequence at 100 MHz OCTACLK. */
    R_XSPI0->LIOCFGCS_b[0].SDRSMPMD = 1U;

    model_source_status = crc32_ieee_calculate(g_wheat_model_image,
                                                RUHMI_MODEL_DATA_BYTES,
                                                &g_storage_info.source_crc32);
    (void) memcpy(sub_0004_model_data,
                  g_wheat_model_image,
                  RUHMI_MODEL_DATA_BYTES);
    model_runtime_status = crc32_ieee_calculate(sub_0004_model_data,
                                                 RUHMI_MODEL_DATA_BYTES,
                                                 &g_storage_info.runtime_crc32);

    command_source_status = crc32_ieee_calculate(
        g_wheat_command_stream_image,
        RUHMI_COMMAND_STREAM_BYTES,
        &g_storage_info.command_stream_source_crc32);
    (void) memcpy(sub_0004_command_stream,
                  g_wheat_command_stream_image,
                  RUHMI_COMMAND_STREAM_BYTES);
    command_runtime_status = crc32_ieee_calculate(
        sub_0004_command_stream,
        RUHMI_COMMAND_STREAM_BYTES,
        &g_storage_info.command_stream_runtime_crc32);

    if ((APP_STATUS_OK != model_source_status) ||
        (APP_STATUS_OK != model_runtime_status) ||
        (APP_STATUS_OK != command_source_status) ||
        (APP_STATUS_OK != command_runtime_status))
    {
        app_status_t const status =
            (APP_STATUS_OK != model_source_status) ? model_source_status :
            (APP_STATUS_OK != model_runtime_status) ? model_runtime_status :
            (APP_STATUS_OK != command_source_status) ? command_source_status : command_runtime_status;

        return wheat_model_storage_fail(status);
    }
    if ((WHEAT_MODEL_IMAGE_CRC32 != g_storage_info.source_crc32) ||
        (WHEAT_MODEL_IMAGE_CRC32 != g_storage_info.runtime_crc32) ||
        (WHEAT_COMMAND_STREAM_CRC32 != g_storage_info.command_stream_source_crc32) ||
        (WHEAT_COMMAND_STREAM_CRC32 != g_storage_info.command_stream_runtime_crc32))
    {
        return wheat_model_storage_fail(APP_STATUS_DATA);
    }

    g_storage_info.copied = true;
    return APP_STATUS_OK;
}

static app_status_t wheat_model_storage_fail (app_status_t status)
{
    if (g_storage_info.ospi_open)
    {
        fsp_err_t const err = g_ospi0.p_api->close(g_ospi0.p_ctrl);

        g_storage_info.fsp_status = (int32_t) err;
        if (FSP_SUCCESS != err)
        {
            return APP_STATUS_IO;
        }
        g_storage_info.ospi_open = false;
    }

    return status;
}

void wheat_model_storage_get_info (wheat_model_storage_info_t * p_info)
{
    if (NULL != p_info)
    {
        *p_info = g_storage_info;
    }
}

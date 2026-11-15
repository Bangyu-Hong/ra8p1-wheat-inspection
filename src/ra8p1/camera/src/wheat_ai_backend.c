#include "wheat_ai_backend.h"

#include <stddef.h>
#include <string.h>

#define WHEAT_FRAME_WIDTH              (640U)
#define WHEAT_FRAME_HEIGHT             (480U)

#if defined(__arm__) || defined(__thumb__)

#include "hal_data.h"
#include "crc32_ieee.h"
#include "platform_time.h"
#include "ruhmi_model_api.h"
#include "wheat_model_storage.h"
#include "wheat_preprocess.h"
#include "wheat_yolox_decode.h"

#define WHEAT_SOURCE_SANITY_OUTPUT_CRC32       (0x6ACE6AFFUL)
#define WHEAT_RA8P1_BOARD_SANITY_OUTPUT_CRC32  (0x04E6D560UL)

typedef struct st_wheat_ai_frame_session
{
    uint8_t const * p_source;
    uint32_t        source_bytes;
    uint16_t        source_width;
    uint16_t        source_height;
    uint16_t        source_stride;
    uint32_t        next_tile_index;
    bool            active;
} wheat_ai_frame_session_t;

static wheat_yolox_accumulator_t g_accumulator;
static wheat_ai_frame_session_t  g_frame_session;
static bool g_crc_acquired;

static wheat_ai_backend_info_t g_backend_info =
{
    .name            = "ruhmi-2.6-ethosu55-tile224",
    .model_identity  = "yolox_tiny_gwhd_224_int8",
    .model_hash      = "7f6a058eb5b4cc4cc61942cdac55a6645fbffe9992731f1f6d72d897338f07f3",
    .last_status     = APP_STATUS_NOT_INITIALIZED,
    .model_available = false,
};

static app_status_t wheat_ai_backend_run_deterministic_sanity(void);
static app_status_t wheat_ai_backend_init_failed(app_status_t status);
static app_status_t wheat_ai_backend_validate_frame(uint8_t const * p_source,
                                                     uint32_t source_bytes,
                                                     uint16_t source_width,
                                                     uint16_t source_height,
                                                     uint16_t source_stride);

app_status_t wheat_ai_backend_init (void)
{
    wheat_model_storage_info_t storage_info;
    app_status_t status;
    fsp_err_t err;

    g_backend_info.requests = 0U;
    g_backend_info.hardware_invocations = 0U;
    g_backend_info.tiles_completed = 0U;
    g_backend_info.last_preprocess_us = 0U;
    g_backend_info.last_inference_us = 0U;
    g_backend_info.last_postprocess_us = 0U;
    g_backend_info.source_sanity_output_crc32 = WHEAT_SOURCE_SANITY_OUTPUT_CRC32;
    g_backend_info.qualified_board_sanity_output_crc32 = WHEAT_RA8P1_BOARD_SANITY_OUTPUT_CRC32;
    g_backend_info.deterministic_sanity_output_crc32 = 0U;
    g_backend_info.storage_source_crc32 = 0U;
    g_backend_info.storage_runtime_crc32 = 0U;
    g_backend_info.command_stream_source_crc32 = 0U;
    g_backend_info.command_stream_runtime_crc32 = 0U;
    g_backend_info.model_available = false;
    g_backend_info.board_numeric_gate_passed = false;
    g_backend_info.frame_active = false;
    g_frame_session = (wheat_ai_frame_session_t) {0};

    if (g_crc_acquired)
    {
        g_backend_info.last_status = APP_STATUS_BUSY;
        return g_backend_info.last_status;
    }

    status = crc32_ieee_open();
    if (APP_STATUS_OK != status)
    {
        g_backend_info.last_status = status;
        return status;
    }
    g_crc_acquired = true;

    status = wheat_model_storage_init();
    wheat_model_storage_get_info(&storage_info);
    g_backend_info.storage_source_address = storage_info.source_address;
    g_backend_info.storage_runtime_address = storage_info.runtime_address;
    g_backend_info.storage_source_crc32 = storage_info.source_crc32;
    g_backend_info.storage_runtime_crc32 = storage_info.runtime_crc32;
    g_backend_info.command_stream_source_address = storage_info.command_stream_source_address;
    g_backend_info.command_stream_runtime_address = storage_info.command_stream_runtime_address;
    g_backend_info.command_stream_source_crc32 = storage_info.command_stream_source_crc32;
    g_backend_info.command_stream_runtime_crc32 = storage_info.command_stream_runtime_crc32;
    g_backend_info.storage_configured_capacity_bytes = storage_info.configured_capacity_bytes;
    g_backend_info.storage_jedec_identity_verified = storage_info.jedec_identity_verified;
    if (APP_STATUS_OK != status)
    {
        return wheat_ai_backend_init_failed(status);
    }

    err = RM_ETHOSU_Open(&g_rm_ethosu0_ctrl, &g_rm_ethosu0_cfg);
    if (FSP_SUCCESS != err)
    {
        return wheat_ai_backend_init_failed(APP_STATUS_IO);
    }

    (void) memset(sub_0004_arena, 0, RUHMI_MODEL_ARENA_BYTES);
    status = wheat_ai_backend_run_deterministic_sanity();
    if (APP_STATUS_OK != status)
    {
        return wheat_ai_backend_init_failed(status);
    }

    g_backend_info.board_numeric_gate_passed = true;
    g_backend_info.model_available = true;
    g_backend_info.last_status = APP_STATUS_OK;
    return APP_STATUS_OK;
}

app_status_t wheat_ai_backend_run_frame (uint8_t const * p_source,
                                         uint32_t source_bytes,
                                         uint16_t source_width,
                                         uint16_t source_height,
                                         uint16_t source_stride,
                                         wheat_detection_result_t * p_result)
{
    app_status_t status;

    if (NULL == p_result)
    {
        g_backend_info.last_status = APP_STATUS_INVALID_ARGUMENT;
        return g_backend_info.last_status;
    }

    *p_result = (wheat_detection_result_t) {0};
    status = wheat_ai_backend_start_frame(p_source,
                                          source_bytes,
                                          source_width,
                                          source_height,
                                          source_stride);
    if (APP_STATUS_OK == status)
    {
        do
        {
            status = wheat_ai_backend_process_frame_step(p_result);
        } while (APP_STATUS_BUSY == status);
    }

    return status;
}

app_status_t wheat_ai_backend_start_frame (uint8_t const * p_source,
                                           uint32_t source_bytes,
                                           uint16_t source_width,
                                           uint16_t source_height,
                                           uint16_t source_stride)
{
    app_status_t const status = wheat_ai_backend_validate_frame(p_source,
                                                                 source_bytes,
                                                                 source_width,
                                                                 source_height,
                                                                 source_stride);

    if (APP_STATUS_OK != status)
    {
        g_backend_info.last_status = status;
        return status;
    }
    if (!g_backend_info.model_available)
    {
        g_backend_info.last_status = APP_STATUS_UNAVAILABLE;
        return g_backend_info.last_status;
    }
    if (g_frame_session.active)
    {
        g_backend_info.last_status = APP_STATUS_BUSY;
        return g_backend_info.last_status;
    }

    g_frame_session = (wheat_ai_frame_session_t)
    {
        .p_source        = p_source,
        .source_bytes    = source_bytes,
        .source_width    = source_width,
        .source_height   = source_height,
        .source_stride   = source_stride,
        .next_tile_index = 0U,
        .active          = true,
    };
    g_backend_info.requests++;
    g_backend_info.tiles_completed = 0U;
    g_backend_info.last_preprocess_us = 0U;
    g_backend_info.last_inference_us = 0U;
    g_backend_info.last_postprocess_us = 0U;
    g_backend_info.frame_active = true;
    g_backend_info.last_status = APP_STATUS_OK;
    wheat_yolox_accumulator_init(&g_accumulator);
    return APP_STATUS_OK;
}

app_status_t wheat_ai_backend_process_frame_step (wheat_detection_result_t * p_result)
{
    int8_t * const p_input = GetModelInputPtr_serving_default_images_0();
    uint16_t tile_x = 0U;
    uint16_t tile_y = 0U;
    uint32_t stage_cycles;
    uint32_t elapsed_us = 0U;
    app_status_t status;

    if (NULL == p_result)
    {
        g_backend_info.last_status = APP_STATUS_INVALID_ARGUMENT;
        return g_backend_info.last_status;
    }
    if (!g_frame_session.active)
    {
        g_backend_info.last_status = APP_STATUS_NOT_INITIALIZED;
        return g_backend_info.last_status;
    }

    status = wheat_yolox_tile_origin(g_frame_session.next_tile_index, &tile_x, &tile_y);
    if (APP_STATUS_OK == status)
    {
        stage_cycles = platform_time_raw_cycles();
        status = wheat_preprocess_rgb565_be_tile_bgr_int8(g_frame_session.p_source,
                                                           g_frame_session.source_bytes,
                                                           g_frame_session.source_width,
                                                           g_frame_session.source_height,
                                                           g_frame_session.source_stride,
                                                           tile_x,
                                                           tile_y,
                                                           WHEAT_YOLOX_TILE_SIZE,
                                                           WHEAT_YOLOX_TILE_SIZE,
                                                           p_input,
                                                           RUHMI_MODEL_INPUT_BYTES);
        (void) platform_time_elapsed_us(stage_cycles,
                                        platform_time_raw_cycles(),
                                        &elapsed_us);
        g_backend_info.last_preprocess_us += elapsed_us;
    }
    if (APP_STATUS_OK == status)
    {
        stage_cycles = platform_time_raw_cycles();
        if (0 != RunModel(false))
        {
            status = APP_STATUS_DATA;
        }
        (void) platform_time_elapsed_us(stage_cycles,
                                        platform_time_raw_cycles(),
                                        &elapsed_us);
        g_backend_info.last_inference_us += elapsed_us;
        if (APP_STATUS_OK == status)
        {
            g_backend_info.hardware_invocations++;
        }
    }
    if (APP_STATUS_OK == status)
    {
        stage_cycles = platform_time_raw_cycles();
        status = wheat_yolox_accumulator_add_tile(
            &g_accumulator,
            GetModelOutputPtr_PartitionedCall_0_70473(),
            RUHMI_MODEL_OUTPUT_BYTES,
            tile_x,
            tile_y);
        (void) platform_time_elapsed_us(stage_cycles,
                                        platform_time_raw_cycles(),
                                        &elapsed_us);
        g_backend_info.last_postprocess_us += elapsed_us;
    }
    if (APP_STATUS_OK != status)
    {
        wheat_ai_backend_cancel_frame(status);
        return status;
    }

    g_frame_session.next_tile_index++;
    g_backend_info.tiles_completed++;
    if (g_frame_session.next_tile_index < WHEAT_YOLOX_TILE_COUNT)
    {
        g_backend_info.last_status = APP_STATUS_BUSY;
        return APP_STATUS_BUSY;
    }

    stage_cycles = platform_time_raw_cycles();
    status = wheat_yolox_accumulator_finalize(&g_accumulator,
                                               g_frame_session.source_width,
                                               g_frame_session.source_height,
                                               p_result);
    (void) platform_time_elapsed_us(stage_cycles,
                                    platform_time_raw_cycles(),
                                    &elapsed_us);
    g_backend_info.last_postprocess_us += elapsed_us;
    g_frame_session = (wheat_ai_frame_session_t) {0};
    g_backend_info.frame_active = false;
    g_backend_info.last_status = status;
    return status;
}

void wheat_ai_backend_cancel_frame (app_status_t status)
{
    g_frame_session = (wheat_ai_frame_session_t) {0};
    g_backend_info.frame_active = false;
    g_backend_info.last_status = status;
}

static app_status_t wheat_ai_backend_validate_frame (uint8_t const * p_source,
                                                      uint32_t source_bytes,
                                                      uint16_t source_width,
                                                      uint16_t source_height,
                                                      uint16_t source_stride)
{
    if ((NULL == p_source) ||
        (WHEAT_FRAME_WIDTH != source_width) ||
        (WHEAT_FRAME_HEIGHT != source_height) ||
        ((uint32_t) source_stride < ((uint32_t) source_width * 2U)) ||
        (((uint64_t) source_stride * source_height) > source_bytes))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    return APP_STATUS_OK;
}

app_status_t wheat_ai_backend_run (uint8_t const * p_input,
                                   uint32_t input_bytes,
                                   wheat_detection_result_t * p_result)
{
    if ((NULL == p_input) || (0U == input_bytes) || (NULL == p_result))
    {
        g_backend_info.last_status = APP_STATUS_INVALID_ARGUMENT;
        return g_backend_info.last_status;
    }

    *p_result = (wheat_detection_result_t) {0};
    g_backend_info.requests++;
    g_backend_info.last_status = APP_STATUS_UNAVAILABLE;
    return g_backend_info.last_status;
}

static app_status_t wheat_ai_backend_run_deterministic_sanity (void)
{
    uint32_t crc32 = 0U;
    app_status_t status;

    (void) memset(GetModelInputPtr_serving_default_images_0(),
                  0x80,
                  RUHMI_MODEL_INPUT_BYTES);
    /* RUHMI aliases output_tensor_0 with the first NPU input region at arena
     * offset zero.  Its clean_outputs path would erase 6174 bytes of that
     * freshly prepared input, so this model must always invoke without it. */
    if (0 != RunModel(false))
    {
        return APP_STATUS_DATA;
    }
    g_backend_info.hardware_invocations++;
    status = crc32_ieee_calculate(
        (uint8_t const *) GetModelOutputPtr_PartitionedCall_0_70473(),
        RUHMI_MODEL_OUTPUT_BYTES,
        &crc32);
    g_backend_info.deterministic_sanity_output_crc32 = crc32;
    /* Source TFLite and generated pre-Vela execution are byte-exact at
     * 0x6ACE6AFF.  Ethos-U55 uses a different deterministic fixed-point
     * signature.  That board signature was accepted only after all 240 frozen
     * tiles preserved 98.06% IoU50 recall, 96.65% precision and count MAE 1.0. */
    if ((APP_STATUS_OK != status) ||
        (WHEAT_RA8P1_BOARD_SANITY_OUTPUT_CRC32 != crc32))
    {
        return (APP_STATUS_OK == status) ? APP_STATUS_DATA : status;
    }

    return APP_STATUS_OK;
}

static app_status_t wheat_ai_backend_init_failed (app_status_t status)
{
    if (g_crc_acquired)
    {
        (void) crc32_ieee_close();
        g_crc_acquired = false;
    }
    g_backend_info.model_available = false;
    g_backend_info.board_numeric_gate_passed = false;
    g_backend_info.last_status = status;
    return status;
}

#else

static wheat_ai_backend_info_t g_backend_info =
{
    .name            = "ruhmi-ethosu55-host-stub",
    .model_identity  = "embedded-only",
    .model_hash      = "none",
    .last_status     = APP_STATUS_NOT_INITIALIZED,
    .model_available = false,
};

app_status_t wheat_ai_backend_init (void)
{
    g_backend_info.requests = 0U;
    g_backend_info.hardware_invocations = 0U;
    g_backend_info.tiles_completed = 0U;
    g_backend_info.frame_active = false;
    g_backend_info.model_available = false;
    g_backend_info.last_status = APP_STATUS_UNAVAILABLE;
    return APP_STATUS_UNAVAILABLE;
}

app_status_t wheat_ai_backend_run_frame (uint8_t const * p_source,
                                         uint32_t source_bytes,
                                         uint16_t source_width,
                                         uint16_t source_height,
                                         uint16_t source_stride,
                                         wheat_detection_result_t * p_result)
{
    app_status_t status;

    if (NULL == p_result)
    {
        g_backend_info.last_status = APP_STATUS_INVALID_ARGUMENT;
        return g_backend_info.last_status;
    }

    *p_result = (wheat_detection_result_t) {0};
    status = wheat_ai_backend_start_frame(p_source,
                                          source_bytes,
                                          source_width,
                                          source_height,
                                          source_stride);
    return status;
}

app_status_t wheat_ai_backend_start_frame (uint8_t const * p_source,
                                           uint32_t source_bytes,
                                           uint16_t source_width,
                                           uint16_t source_height,
                                           uint16_t source_stride)
{
    if ((NULL == p_source) ||
        (WHEAT_FRAME_WIDTH != source_width) ||
        (WHEAT_FRAME_HEIGHT != source_height) ||
        ((uint32_t) source_stride < ((uint32_t) source_width * 2U)) ||
        (((uint64_t) source_stride * source_height) > source_bytes))
    {
        g_backend_info.last_status = APP_STATUS_INVALID_ARGUMENT;
        return g_backend_info.last_status;
    }

    g_backend_info.last_status = APP_STATUS_UNAVAILABLE;
    return g_backend_info.last_status;
}

app_status_t wheat_ai_backend_process_frame_step (wheat_detection_result_t * p_result)
{
    if (NULL == p_result)
    {
        g_backend_info.last_status = APP_STATUS_INVALID_ARGUMENT;
        return g_backend_info.last_status;
    }

    g_backend_info.last_status = APP_STATUS_NOT_INITIALIZED;
    return g_backend_info.last_status;
}

void wheat_ai_backend_cancel_frame (app_status_t status)
{
    g_backend_info.frame_active = false;
    g_backend_info.last_status = status;
}

app_status_t wheat_ai_backend_run (uint8_t const * p_input,
                                   uint32_t input_bytes,
                                   wheat_detection_result_t * p_result)
{
    if ((NULL == p_input) || (0U == input_bytes) || (NULL == p_result))
    {
        g_backend_info.last_status = APP_STATUS_INVALID_ARGUMENT;
        return g_backend_info.last_status;
    }

    *p_result = (wheat_detection_result_t) {0};
    g_backend_info.requests++;
    g_backend_info.last_status = APP_STATUS_UNAVAILABLE;
    return g_backend_info.last_status;
}

#endif

void wheat_ai_backend_get_info (wheat_ai_backend_info_t * p_info)
{
    if (NULL != p_info)
    {
        *p_info = g_backend_info;
    }
}

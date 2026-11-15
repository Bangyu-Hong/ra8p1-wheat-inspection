#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ai_interaction.h"
#include "lcd_result_overlay.h"
#include "sdram_test.h"
#include "st7796s_controller.h"
#include "user_button.h"
#include "wheat_ai_backend.h"
#include "wheat_postprocess.h"
#include "wheat_preprocess.h"
#include "wheat_yolox_decode.h"

#define TEST_CHECK(condition)                                                       \
    do                                                                              \
    {                                                                               \
        if (!(condition))                                                           \
        {                                                                           \
            (void) printf("check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                           \
        }                                                                           \
    } while (0)

static bool test_button_irq_latch(void);
static bool test_interaction_cycle(void);
static bool test_sdram_patterns(void);
static bool test_preprocess_letterbox(void);
static bool test_preprocess_tile_bgr_int8(void);
static bool test_postprocess_nms(void);
static bool test_yolox_tile_decode(void);
static bool test_backend_unavailable(void);
static bool test_overlay_contract(void);
static app_status_t test_sdram_abort(sdram_test_phase_t phase,
                                     uint32_t completed_words,
                                     void * p_context);

static uint32_t g_overlay_fill_calls;
static uint32_t g_overlay_write_calls;
static uint32_t g_sdram_progress_calls;
static uint32_t g_sdram_abort_words[65536];

int main (void)
{
    bool const passed = test_button_irq_latch() &&
                        test_interaction_cycle() &&
                        test_sdram_patterns() &&
                        test_preprocess_letterbox() &&
                        test_preprocess_tile_bgr_int8() &&
                        test_postprocess_nms() &&
                        test_yolox_tile_decode() &&
                        test_backend_unavailable() &&
                        test_overlay_contract();

    if (!passed)
    {
        return 1;
    }

    (void) printf("ai-foundations button=ok state=ok sdram=ok preprocess=ok nms=ok\n");
    return 0;
}

static bool test_button_irq_latch (void)
{
    user_button_t button;
    user_button_diagnostics_t diagnostics;

    user_button_init(&button, false, 0U);
    user_button_latch_press_from_isr(&button);
    user_button_latch_press_from_isr(&button);
    user_button_latch_press_from_isr(&button);
    user_button_get_diagnostics(&button, &diagnostics);
    TEST_CHECK(3U == diagnostics.irq_events);
    TEST_CHECK(2U == diagnostics.ignored_irq_events);
    TEST_CHECK(1U == diagnostics.pending_press_events);
    TEST_CHECK(USER_BUTTON_EVENT_PRESSED == user_button_consume_press(&button));
    TEST_CHECK(USER_BUTTON_EVENT_NONE == user_button_consume_press(&button));
    TEST_CHECK(1U == button.press_events);
    TEST_CHECK(1U == button.consumed_events);

    TEST_CHECK(USER_BUTTON_EVENT_NONE == user_button_update(&button, true, 1000000U));
    TEST_CHECK(USER_BUTTON_EVENT_NONE == user_button_update(&button, false, 1001000U));
    TEST_CHECK(USER_BUTTON_EVENT_NONE == user_button_update(&button, false, 1030999U));
    TEST_CHECK(USER_BUTTON_EVENT_RELEASED == user_button_update(&button, false, 1031000U));
    TEST_CHECK(1U == button.release_events);
    TEST_CHECK(1U == button.press_armed);

    user_button_latch_press_from_isr(&button);
    TEST_CHECK(USER_BUTTON_EVENT_PRESSED == user_button_consume_press(&button));
    TEST_CHECK(2U == button.press_events);
    TEST_CHECK(2U == button.consumed_events);
    user_button_record_ignored_busy(&button);
    TEST_CHECK(1U == button.ignored_busy_events);

    user_button_init(&button, true, 0U);
    user_button_latch_press_from_isr(&button);
    user_button_get_diagnostics(&button, &diagnostics);
    TEST_CHECK(1U == diagnostics.irq_events);
    TEST_CHECK(1U == diagnostics.ignored_irq_events);
    TEST_CHECK(USER_BUTTON_EVENT_NONE == user_button_consume_press(&button));
    TEST_CHECK(USER_BUTTON_EVENT_NONE == user_button_update(&button, true, 50000U));
    TEST_CHECK(USER_BUTTON_EVENT_NONE == user_button_update(&button, false, 60000U));
    TEST_CHECK(USER_BUTTON_EVENT_RELEASED == user_button_update(&button, false, 90000U));
    user_button_latch_press_from_isr(&button);
    TEST_CHECK(USER_BUTTON_EVENT_PRESSED == user_button_consume_press(&button));

    button.irq_events = UINT32_MAX;
    button.ignored_irq_events = UINT32_MAX;
    user_button_latch_press_from_isr(&button);
    user_button_get_diagnostics(&button, &diagnostics);
    TEST_CHECK(UINT32_MAX == diagnostics.irq_events);
    TEST_CHECK(UINT32_MAX == diagnostics.ignored_irq_events);

    return true;
}

static bool test_interaction_cycle (void)
{
    ai_interaction_t interaction;

    ai_interaction_init(&interaction);
    TEST_CHECK(AI_INTERACTION_LIVE_PREVIEW == interaction.state);
    TEST_CHECK(AI_INTERACTION_ACTION_REQUEST_FREEZE == ai_interaction_on_press(&interaction));
    TEST_CHECK(AI_INTERACTION_ACTION_REPORT_BUSY == ai_interaction_on_press(&interaction));
    TEST_CHECK(AI_INTERACTION_ACTION_RUN_INFERENCE == ai_interaction_on_frame_held(&interaction));
    TEST_CHECK(AI_INTERACTION_ACTION_REPORT_BUSY == ai_interaction_on_press(&interaction));
    TEST_CHECK(AI_INTERACTION_ACTION_PRESENT_RESULT ==
               ai_interaction_on_inference_complete(&interaction, APP_STATUS_UNAVAILABLE));
    TEST_CHECK(AI_INTERACTION_RESULT_HOLD == interaction.state);
    TEST_CHECK(AI_INTERACTION_ACTION_RESUME_PREVIEW == ai_interaction_on_press(&interaction));
    TEST_CHECK(AI_INTERACTION_RESULT_HOLD == interaction.state);
    TEST_CHECK(AI_INTERACTION_ACTION_NONE == ai_interaction_on_resume_complete(&interaction));
    TEST_CHECK(AI_INTERACTION_LIVE_PREVIEW == interaction.state);
    TEST_CHECK(1U == interaction.freeze_requests);
    TEST_CHECK(1U == interaction.inference_requests);
    TEST_CHECK(1U == interaction.completed_results);
    TEST_CHECK(1U == interaction.resume_requests);
    TEST_CHECK(2U == interaction.busy_presses);

    ai_interaction_init(&interaction);
    TEST_CHECK(AI_INTERACTION_ACTION_REQUEST_FREEZE == ai_interaction_on_press(&interaction));
    TEST_CHECK(AI_INTERACTION_ACTION_NONE ==
               ai_interaction_cancel_freeze(&interaction, APP_STATUS_IO));
    TEST_CHECK(AI_INTERACTION_LIVE_PREVIEW == interaction.state);
    TEST_CHECK(APP_STATUS_IO == interaction.result_status);

    TEST_CHECK(AI_INTERACTION_ACTION_REQUEST_FREEZE == ai_interaction_on_press(&interaction));
    TEST_CHECK(AI_INTERACTION_ACTION_NONE ==
               ai_interaction_on_transaction_failed(&interaction, APP_STATUS_BUSY));
    TEST_CHECK(AI_INTERACTION_FREEZE_PENDING == interaction.state);
    TEST_CHECK(0U == interaction.completed_results);
    TEST_CHECK(AI_INTERACTION_ACTION_PRESENT_RESULT ==
               ai_interaction_on_transaction_failed(&interaction, APP_STATUS_DATA));
    TEST_CHECK(AI_INTERACTION_RESULT_HOLD == interaction.state);
    TEST_CHECK(AI_INTERACTION_ACTION_RESUME_PREVIEW == ai_interaction_on_press(&interaction));
    TEST_CHECK(AI_INTERACTION_RESULT_HOLD == interaction.state);

    return true;
}

static bool test_sdram_patterns (void)
{
    uint32_t words[256];
    sdram_test_result_t result;

    (void) memset(words, 0, sizeof(words));
    TEST_CHECK(APP_STATUS_OK == sdram_test_run(words,
                                               (uint32_t) sizeof(words),
                                               NULL,
                                               NULL,
                                               &result));
    TEST_CHECK(result.passed);
    TEST_CHECK(SDRAM_TEST_PHASE_COMPLETE == result.phase);
    TEST_CHECK((uint32_t) sizeof(words) == result.bytes_tested);
    TEST_CHECK(result.words_verified > 256U);
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == sdram_test_run(words,
                                                             (uint32_t) sizeof(words) - 4U,
                                                             NULL,
                                                             NULL,
                                                             &result));
    g_sdram_progress_calls = 0U;
    TEST_CHECK(APP_STATUS_TIMEOUT == sdram_test_run(g_sdram_abort_words,
                                                    (uint32_t) sizeof(g_sdram_abort_words),
                                                    test_sdram_abort,
                                                    NULL,
                                                    &result));
    TEST_CHECK(1U == g_sdram_progress_calls);
    TEST_CHECK(SDRAM_TEST_PHASE_CHECKERBOARD_A == result.phase);
    TEST_CHECK(!result.passed);

    return true;
}

static bool test_preprocess_letterbox (void)
{
    uint8_t const source[] =
    {
        0xF8U, 0x00U, 0x07U, 0xE0U, 0x00U, 0x1FU, 0xFFU, 0xFFU,
        0x00U, 0x00U, 0xFFU, 0xFFU, 0xF8U, 0x00U, 0x07U, 0xE0U,
    };
    uint8_t destination[4U * 4U * 3U];
    wheat_preprocess_geometry_t geometry;
    wheat_preprocess_config_t const config =
    {
        .target_width     = 4U,
        .target_height    = 4U,
        .padding_rgb      = 114U,
        .layout           = WHEAT_TENSOR_LAYOUT_NHWC,
        .type             = WHEAT_TENSOR_UINT8,
        .source_scale     = 1.0F,
        .tensor_scale     = 1.0F,
        .tensor_zero_point = 0,
    };

    TEST_CHECK(APP_STATUS_OK == wheat_preprocess_rgb565_be(source,
                                                            (uint32_t) sizeof(source),
                                                            4U,
                                                            2U,
                                                            8U,
                                                            &config,
                                                            destination,
                                                            (uint32_t) sizeof(destination),
                                                            &geometry));
    TEST_CHECK(4U == geometry.scaled_width);
    TEST_CHECK(2U == geometry.scaled_height);
    TEST_CHECK(0U == geometry.offset_x);
    TEST_CHECK(1U == geometry.offset_y);
    for (uint32_t index = 0U; index < 12U; index++)
    {
        TEST_CHECK(114U == destination[index]);
        TEST_CHECK(114U == destination[36U + index]);
    }
    TEST_CHECK(255U == destination[12]);
    TEST_CHECK(0U == destination[13]);
    TEST_CHECK(0U == destination[14]);
    TEST_CHECK(0U == destination[15]);
    TEST_CHECK(255U == destination[16]);
    TEST_CHECK(0U == destination[17]);

    return true;
}

static bool test_preprocess_tile_bgr_int8 (void)
{
    uint8_t const source[] =
    {
        0xF8U, 0x00U, 0x07U, 0xE0U, 0x00U, 0x1FU,
        0xFFU, 0xFFU, 0x00U, 0x00U, 0xF8U, 0x00U,
    };
    int8_t destination[2U * 2U * 3U];

    TEST_CHECK(APP_STATUS_OK == wheat_preprocess_rgb565_be_tile_bgr_int8(
        source,
        (uint32_t) sizeof(source),
        3U,
        2U,
        6U,
        1U,
        0U,
        2U,
        2U,
        destination,
        (uint32_t) sizeof(destination)));
    TEST_CHECK(-128 == destination[0]);
    TEST_CHECK(127 == destination[1]);
    TEST_CHECK(-128 == destination[2]);
    TEST_CHECK(127 == destination[3]);
    TEST_CHECK(-128 == destination[4]);
    TEST_CHECK(-128 == destination[5]);
    TEST_CHECK(-128 == destination[6]);
    TEST_CHECK(-128 == destination[7]);
    TEST_CHECK(-128 == destination[8]);
    TEST_CHECK(-128 == destination[9]);
    TEST_CHECK(-128 == destination[10]);
    TEST_CHECK(127 == destination[11]);
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT ==
               wheat_preprocess_rgb565_be_tile_bgr_int8(source,
                                                         (uint32_t) sizeof(source),
                                                         3U,
                                                         2U,
                                                         6U,
                                                         2U,
                                                         0U,
                                                         2U,
                                                         2U,
                                                         destination,
                                                         (uint32_t) sizeof(destination)));

    return true;
}

static bool test_yolox_tile_decode (void)
{
    uint16_t const expected_x[WHEAT_YOLOX_TILE_COUNT] =
    {
        0U, 179U, 358U, 416U,
        0U, 179U, 358U, 416U,
        0U, 179U, 358U, 416U,
    };
    uint16_t const expected_y[WHEAT_YOLOX_TILE_COUNT] =
    {
        0U, 0U, 0U, 0U,
        179U, 179U, 179U, 179U,
        256U, 256U, 256U, 256U,
    };
    int8_t output[WHEAT_YOLOX_OUTPUT_BYTES];
    wheat_yolox_accumulator_t accumulator;
    wheat_detection_result_t result;

    for (uint32_t index = 0U; index < WHEAT_YOLOX_TILE_COUNT; index++)
    {
        uint16_t tile_x = UINT16_MAX;
        uint16_t tile_y = UINT16_MAX;

        TEST_CHECK(APP_STATUS_OK == wheat_yolox_tile_origin(index, &tile_x, &tile_y));
        TEST_CHECK(expected_x[index] == tile_x);
        TEST_CHECK(expected_y[index] == tile_y);
    }
    {
        uint16_t tile_x = UINT16_MAX;
        uint16_t tile_y = UINT16_MAX;

        TEST_CHECK(APP_STATUS_INVALID_ARGUMENT ==
                   wheat_yolox_tile_origin(WHEAT_YOLOX_TILE_COUNT, &tile_x, &tile_y));
        TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == wheat_yolox_tile_origin(0U, NULL, &tile_y));
        TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == wheat_yolox_tile_origin(0U, &tile_x, NULL));
    }

    (void) memset(output, 0x80, sizeof(output));
    output[0] = -48;
    output[1] = -48;
    output[2] = -48;
    output[3] = -48;
    /* -21 dequantizes slightly above one with the tensor-wide scale.  The
     * probability channels must saturate instead of dropping this row. */
    output[4] = -21;
    output[5] = -21;

    wheat_yolox_accumulator_init(&accumulator);
    TEST_CHECK(APP_STATUS_OK == wheat_yolox_accumulator_add_tile(&accumulator,
                                                                  output,
                                                                  (uint32_t) sizeof(output),
                                                                  10U,
                                                                  20U));
    TEST_CHECK(APP_STATUS_OK == wheat_yolox_accumulator_finalize(&accumulator,
                                                                  640U,
                                                                  480U,
                                                                  &result));
    TEST_CHECK(WHEAT_YOLOX_OUTPUT_ROWS == result.candidates_scanned);
    TEST_CHECK(1U == result.candidates_above_threshold);
    TEST_CHECK(1U == result.count);
    TEST_CHECK(6U == result.detections[0].x1);
    TEST_CHECK(16U == result.detections[0].y1);
    TEST_CHECK(14U == result.detections[0].x2);
    TEST_CHECK(24U == result.detections[0].y2);

    /* Two invalid negative probability channels must not multiply into a
     * positive score.  -55 dequantizes to about -0.264 for both channels. */
    (void) memset(output, 0x80, sizeof(output));
    output[0] = -48;
    output[1] = -48;
    output[2] = -48;
    output[3] = -48;
    output[4] = -55;
    output[5] = -55;
    wheat_yolox_accumulator_init(&accumulator);
    TEST_CHECK(APP_STATUS_OK == wheat_yolox_accumulator_add_tile(&accumulator,
                                                                  output,
                                                                  (uint32_t) sizeof(output),
                                                                  10U,
                                                                  20U));
    TEST_CHECK(APP_STATUS_OK == wheat_yolox_accumulator_finalize(&accumulator,
                                                                  640U,
                                                                  480U,
                                                                  &result));
    TEST_CHECK(0U == result.candidates_above_threshold);
    TEST_CHECK(0U == result.count);

    return true;
}

static bool test_postprocess_nms (void)
{
    wheat_candidate_t const candidates[] =
    {
        {.center_x = 320.0F, .center_y = 320.0F, .width = 100.0F, .height = 100.0F, .score = 0.90F},
        {.center_x = 322.0F, .center_y = 321.0F, .width = 100.0F, .height = 100.0F, .score = 0.80F},
        {.center_x = 20.0F, .center_y = 90.0F, .width = 80.0F, .height = 40.0F, .score = 0.70F},
        {.center_x = 500.0F, .center_y = 400.0F, .width = 20.0F, .height = 20.0F, .score = 0.10F},
    };
    wheat_preprocess_geometry_t const geometry =
    {
        .source_width  = 640U,
        .source_height = 480U,
        .target_width  = 640U,
        .target_height = 640U,
        .scaled_width  = 640U,
        .scaled_height = 480U,
        .offset_x      = 0U,
        .offset_y      = 80U,
        .output_bytes  = 1228800U,
    };
    wheat_postprocess_config_t const config =
    {
        .confidence_threshold = 0.25F,
        .iou_threshold        = 0.45F,
        .source_width         = 640U,
        .source_height        = 480U,
    };
    wheat_detection_result_t result;

    TEST_CHECK(APP_STATUS_OK == wheat_postprocess_candidates(candidates,
                                                              (uint32_t) (sizeof(candidates) /
                                                                          sizeof(candidates[0])),
                                                              &geometry,
                                                              &config,
                                                              &result));
    TEST_CHECK(4U == result.candidates_scanned);
    TEST_CHECK(3U == result.candidates_above_threshold);
    TEST_CHECK(2U == result.count);
    TEST_CHECK(270U == result.detections[0].x1);
    TEST_CHECK(190U == result.detections[0].y1);
    TEST_CHECK(370U == result.detections[0].x2);
    TEST_CHECK(290U == result.detections[0].y2);
    TEST_CHECK(0U == result.detections[1].x1);
    TEST_CHECK(0U == result.detections[1].y1);

    {
        uint32_t nan_bits = 0x7FC00000UL;
        wheat_candidate_t malformed[2] =
        {
            {.center_x = 10.0F, .center_y = 100.0F, .width = 5.0F, .height = 5.0F, .score = 0.8F},
            {.center_x = 10.0F, .center_y = 100.0F, .width = 5.0F, .height = 5.0F, .score = 0.8F},
        };

        (void) memcpy(&malformed[0].score, &nan_bits, sizeof(nan_bits));
        (void) memcpy(&malformed[1].width, &nan_bits, sizeof(nan_bits));
        TEST_CHECK(APP_STATUS_OK == wheat_postprocess_candidates(malformed,
                                                                  2U,
                                                                  &geometry,
                                                                  &config,
                                                                  &result));
        TEST_CHECK(0U == result.candidates_above_threshold);
        TEST_CHECK(0U == result.count);
    }

    {
        static wheat_candidate_t capacity_filter[WHEAT_POSTPROCESS_MAX_CANDIDATES + 1U];

        for (uint32_t index = 0U; index < WHEAT_POSTPROCESS_MAX_CANDIDATES; index++)
        {
            capacity_filter[index] = (wheat_candidate_t)
            {
                .center_x = 100.0F,
                .center_y = 20.0F,
                .width    = 10.0F,
                .height   = 10.0F,
                .score    = 0.9F,
            };
        }
        capacity_filter[WHEAT_POSTPROCESS_MAX_CANDIDATES] = (wheat_candidate_t)
        {
            .center_x = 320.0F,
            .center_y = 320.0F,
            .width    = 20.0F,
            .height   = 20.0F,
            .score    = 0.5F,
        };
        TEST_CHECK(APP_STATUS_OK == wheat_postprocess_candidates(
            capacity_filter,
            WHEAT_POSTPROCESS_MAX_CANDIDATES + 1U,
            &geometry,
            &config,
            &result));
        TEST_CHECK((WHEAT_POSTPROCESS_MAX_CANDIDATES + 1U) ==
                   result.candidates_above_threshold);
        TEST_CHECK(0U == result.candidates_dropped);
        TEST_CHECK(1U == result.count);
        TEST_CHECK(310U == result.detections[0].x1);
        TEST_CHECK(230U == result.detections[0].y1);
    }

    {
        /* The frozen INT8 validation contains frames with 82 detections.  The
         * product result container must retain that known dense-frame case. */
        enum { TEST_DENSE_DETECTIONS = 82U };
        wheat_candidate_t dense[TEST_DENSE_DETECTIONS];

        TEST_CHECK(WHEAT_POSTPROCESS_MAX_RESULTS >= TEST_DENSE_DETECTIONS);
        for (uint32_t index = 0U; index < TEST_DENSE_DETECTIONS; index++)
        {
            dense[index] = (wheat_candidate_t)
            {
                .center_x = 5.0F + (float) ((index % 10U) * 60U),
                .center_y = 82.0F + (float) ((index / 10U) * 45U),
                .width = 4.0F,
                .height = 4.0F,
                .score = 0.9F,
            };
        }
        TEST_CHECK(APP_STATUS_OK == wheat_postprocess_candidates(
            dense,
            TEST_DENSE_DETECTIONS,
            &geometry,
            &config,
            &result));
        TEST_CHECK(TEST_DENSE_DETECTIONS == result.count);
        TEST_CHECK(!result.result_overflow);
        TEST_CHECK(0U == result.detections_dropped);
    }

    return true;
}

static bool test_backend_unavailable (void)
{
    uint8_t input = 0U;
    wheat_detection_result_t result = {.count = 1U};
    wheat_ai_backend_info_t info;

    TEST_CHECK(APP_STATUS_UNAVAILABLE == wheat_ai_backend_init());
    TEST_CHECK(APP_STATUS_UNAVAILABLE == wheat_ai_backend_run(&input, 1U, &result));
    TEST_CHECK(0U == result.count);
    TEST_CHECK(0U == result.candidates_scanned);
    wheat_ai_backend_get_info(&info);
    TEST_CHECK(!info.model_available);
    TEST_CHECK(1U == info.requests);
    TEST_CHECK(0U == info.hardware_invocations);
    TEST_CHECK(0 == strcmp("none", info.model_hash));
    TEST_CHECK(!info.frame_active);

    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT ==
               wheat_ai_backend_start_frame(&input, 1U, 1U, 1U, 2U));
    TEST_CHECK(APP_STATUS_UNAVAILABLE ==
               wheat_ai_backend_start_frame(&input, 614400U, 640U, 480U, 1280U));
    TEST_CHECK(APP_STATUS_NOT_INITIALIZED ==
               wheat_ai_backend_process_frame_step(&result));
    wheat_ai_backend_cancel_frame(APP_STATUS_TIMEOUT);
    wheat_ai_backend_get_info(&info);
    TEST_CHECK(!info.frame_active);
    TEST_CHECK(APP_STATUS_TIMEOUT == info.last_status);

    return true;
}

static bool test_overlay_contract (void)
{
    wheat_detection_result_t result = {0};
    lcd_overlay_box_t box;

    TEST_CHECK(APP_STATUS_OK == lcd_result_overlay_map_box(
        &(wheat_detection_t) {.x1 = 0U, .y1 = 0U, .x2 = 640U, .y2 = 480U},
        &box));
    TEST_CHECK(27U == box.x);
    TEST_CHECK(0U == box.y);
    TEST_CHECK(426U == box.width);
    TEST_CHECK(320U == box.height);

    result.count = 1U;
    result.detections[0] = (wheat_detection_t)
    {
        .x1 = 100U, .y1 = 100U, .x2 = 200U, .y2 = 200U, .score = 0.9F,
    };
    g_overlay_fill_calls = 0U;
    g_overlay_write_calls = 0U;
    TEST_CHECK(APP_STATUS_UNAVAILABLE == wheat_ai_backend_init());
    TEST_CHECK(APP_STATUS_OK == lcd_result_overlay_draw(&result, APP_STATUS_UNAVAILABLE));
    TEST_CHECK(0U == g_overlay_fill_calls);
    TEST_CHECK(1U == g_overlay_write_calls);

    result.count = WHEAT_POSTPROCESS_MAX_RESULTS + 1U;
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == lcd_result_overlay_draw(&result, APP_STATUS_OK));
    TEST_CHECK(0U == g_overlay_fill_calls);
    TEST_CHECK(1U == g_overlay_write_calls);

    return true;
}

static app_status_t test_sdram_abort (sdram_test_phase_t phase,
                                      uint32_t completed_words,
                                      void * p_context)
{
    (void) phase;
    (void) completed_words;
    (void) p_context;
    g_sdram_progress_calls++;
    return APP_STATUS_TIMEOUT;
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
    g_overlay_fill_calls++;
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
    if (NULL == p_pixels)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    g_overlay_write_calls++;
    return APP_STATUS_OK;
}

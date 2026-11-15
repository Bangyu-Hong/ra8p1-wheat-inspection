#include "app_state.h"

#include "app_config.h"
#include "board_port.h"
#include "camera_ov5640.h"
#include "dht11.h"
#include "esp8266_link.h"
#include "hmi_uart.h"
#include "light_adc.h"
#include "storage.h"
#include "tile_inference.h"
#include "wheat_ai.h"

#include <string.h>

static void enter_error(app_context_t * context, app_status_t error, const char * message)
{
    context->last_error = error;
    context->state = APP_STATE_ERROR;
    (void) hmi_set_status(message);
}

static void prepare_record(app_context_t * context)
{
    memset(&context->record, 0, sizeof(context->record));
    strncpy(context->record.sample_id, context->sample_id, sizeof(context->record.sample_id) - 1U);
    context->record.timestamp_s = board_rtc_timestamp_s();
    context->record.wheat_ear_count = (uint16_t) context->detection_count;
    context->record.environment = context->environment;
    context->record.light_percent = context->light_percent;
    context->record.image_quality = context->quality;
}

void app_state_init(app_context_t * context, uint8_t * frame_buffer, size_t frame_buffer_size)
{
    size_t required = (size_t) CAMERA_FRAME_STRIDE * CAMERA_FRAME_HEIGHT;
    if (context == NULL)
    {
        return;
    }

    memset(context, 0, sizeof(*context));
    context->state = APP_STATE_BOOT;
    if ((frame_buffer == NULL) || (frame_buffer_size < required))
    {
        context->state = APP_STATE_ERROR;
        context->last_error = APP_ERR_NO_SPACE;
        return;
    }

    context->frame.data = frame_buffer;
    context->frame.width = CAMERA_FRAME_WIDTH;
    context->frame.height = CAMERA_FRAME_HEIGHT;
    context->frame.stride_bytes = CAMERA_FRAME_STRIDE;
    context->frame.format = PIXEL_FORMAT_RGB565;
}

void app_state_step(app_context_t * context)
{
    app_status_t status;
    hmi_event_t event;

    if (context == NULL)
    {
        return;
    }

    switch (context->state)
    {
        case APP_STATE_BOOT:
            (void) hmi_init();
            (void) hmi_set_status("initializing");
            status = camera_ov5640_init();
            if (status != APP_OK)
            {
                enter_error(context, status, "camera_init_failed");
                break;
            }
            status = wheat_ai_init();
            if (status != APP_OK)
            {
                enter_error(context, status, "model_init_failed");
                break;
            }
            context->storage_available = (storage_init() == APP_OK);
            context->network_available = (esp8266_link_init() == APP_OK);
            context->state = APP_STATE_IDLE;
            (void) hmi_set_status("ready");
            break;

        case APP_STATE_IDLE:
            event = hmi_poll_event(context->sample_id, sizeof(context->sample_id));
            if (event == HMI_EVENT_START)
            {
                context->last_error = APP_OK;
                context->state = APP_STATE_ENV_CHECK;
                (void) hmi_set_status("checking_environment");
            }
            break;

        case APP_STATE_ENV_CHECK:
            status = dht11_read(&context->environment);
            if (status != APP_OK)
            {
                /* DHT11 is metadata only; retain the result with valid=false. */
                context->environment.valid = false;
            }
            status = light_adc_read_percent(&context->light_percent);
            if (status != APP_OK)
            {
                enter_error(context, status, "light_sensor_failed");
                break;
            }
            if (!light_adc_is_acceptable(context->light_percent))
            {
                enter_error(context, APP_ERR_BAD_IMAGE, "adjust_lighting");
                break;
            }
            context->state = APP_STATE_CAPTURE;
            (void) hmi_set_status("capturing");
            break;

        case APP_STATE_CAPTURE:
            status = camera_ov5640_capture(&context->frame, CAMERA_CAPTURE_TIMEOUT_MS);
            if (status != APP_OK)
            {
                enter_error(context, status, "camera_timeout");
                break;
            }
            context->state = APP_STATE_QUALITY_CHECK;
            break;

        case APP_STATE_QUALITY_CHECK:
            status = camera_check_image_quality(&context->frame, &context->quality);
            if (status != APP_OK)
            {
                enter_error(context, status, "image_check_failed");
                break;
            }
            if (context->quality.code != IMAGE_QUALITY_OK)
            {
                enter_error(context, APP_ERR_BAD_IMAGE, "retake_required");
                break;
            }
            context->state = APP_STATE_INFERENCE;
            (void) hmi_set_status("detecting");
            break;

        case APP_STATE_INFERENCE:
            status = tile_inference_run(&context->frame,
                                        context->detections,
                                        APP_MAX_DETECTIONS,
                                        &context->detection_count);
            if (status != APP_OK)
            {
                enter_error(context, status, "inference_failed");
                break;
            }
            prepare_record(context);
            context->state = APP_STATE_SAVE;
            break;

        case APP_STATE_SAVE:
            if (context->storage_available)
            {
                status = storage_save_record(&context->record);
                if (status == APP_OK)
                {
                    status = storage_save_rgb565(context->sample_id, &context->frame);
                }
                if (status != APP_OK)
                {
                    context->storage_available = false;
                    (void) hmi_set_status("storage_unavailable");
                }
            }
            context->state = APP_STATE_UPLOAD;
            break;

        case APP_STATE_UPLOAD:
            if (!esp8266_link_is_online())
            {
                status = esp8266_link_init();
            }
            else
            {
                status = APP_OK;
            }
            if (status == APP_OK)
            {
                status = esp8266_send_record(&context->record);
            }
            context->network_available = (status == APP_OK);
            if ((status != APP_OK) && context->storage_available)
            {
                (void) storage_cache_pending_upload(&context->record);
            }
            context->state = APP_STATE_DONE;
            break;

        case APP_STATE_DONE:
            (void) hmi_show_result(&context->record);
            (void) hmi_set_status(context->network_available ? "complete" : "complete_offline");
            context->state = APP_STATE_IDLE;
            break;

        case APP_STATE_ERROR:
            event = hmi_poll_event(context->sample_id, sizeof(context->sample_id));
            if (event == HMI_EVENT_START)
            {
                context->last_error = APP_OK;
                context->state = APP_STATE_ENV_CHECK;
            }
            else if (event == HMI_EVENT_CANCEL)
            {
                context->state = APP_STATE_IDLE;
                (void) hmi_set_status("ready");
            }
            break;

        default:
            enter_error(context, APP_ERR_PROTOCOL, "invalid_state");
            break;
    }
}

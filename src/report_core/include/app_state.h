#ifndef APP_STATE_H
#define APP_STATE_H

#include "app_config.h"
#include "common_types.h"

typedef enum
{
    APP_STATE_BOOT = 0,
    APP_STATE_IDLE,
    APP_STATE_ENV_CHECK,
    APP_STATE_CAPTURE,
    APP_STATE_QUALITY_CHECK,
    APP_STATE_INFERENCE,
    APP_STATE_SAVE,
    APP_STATE_UPLOAD,
    APP_STATE_DONE,
    APP_STATE_ERROR
} app_state_id_t;

typedef struct
{
    app_state_id_t state;
    app_status_t last_error;
    bool storage_available;
    bool network_available;
    char sample_id[24];
    image_t frame;
    environment_t environment;
    uint8_t light_percent;
    image_quality_t quality;
    detection_t detections[APP_MAX_DETECTIONS];
    size_t detection_count;
    detection_record_t record;
} app_context_t;

void app_state_init(app_context_t * context, uint8_t * frame_buffer, size_t frame_buffer_size);
void app_state_step(app_context_t * context);

#endif

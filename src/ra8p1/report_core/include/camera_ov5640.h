#ifndef CAMERA_OV5640_H
#define CAMERA_OV5640_H

#include "common_types.h"

app_status_t camera_ov5640_init(void);
app_status_t camera_ov5640_capture(image_t * frame, uint32_t timeout_ms);
app_status_t camera_check_image_quality(const image_t * frame, image_quality_t * quality);

#endif

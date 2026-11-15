#ifndef WHEAT_AI_H
#define WHEAT_AI_H

#include "common_types.h"

app_status_t wheat_ai_init(void);
app_status_t wheat_ai_infer_tile(const image_t * frame,
                                 image_rect_t tile,
                                 detection_t * detections,
                                 size_t capacity,
                                 size_t * count);

#endif

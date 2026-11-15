#ifndef TILE_INFERENCE_H
#define TILE_INFERENCE_H

#include "common_types.h"

app_status_t tile_inference_run(const image_t * frame,
                                detection_t * global_detections,
                                size_t capacity,
                                size_t * count);
size_t tile_global_nms(detection_t * detections, size_t count, float iou_threshold);

#endif

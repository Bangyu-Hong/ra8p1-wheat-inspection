#ifndef WHEAT_MODEL_PORT_H
#define WHEAT_MODEL_PORT_H

#include "common_types.h"

typedef struct
{
    float center_x;
    float center_y;
    float width;
    float height;
    float score;
    uint8_t class_id;
} model_detection_t;

app_status_t wheat_model_port_init(void);
app_status_t wheat_model_port_invoke(const int8_t * input,
                                     size_t input_length,
                                     model_detection_t * output,
                                     size_t output_capacity,
                                     size_t * output_count);

#endif

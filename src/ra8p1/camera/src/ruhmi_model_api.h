#ifndef RUHMI_MODEL_API_H_
#define RUHMI_MODEL_API_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RUHMI_MODEL_INPUT_BYTES        (150528U)
#define RUHMI_MODEL_OUTPUT_BYTES       (6174U)
#define RUHMI_MODEL_DATA_BYTES         (4625552U)
#define RUHMI_MODEL_ARENA_BYTES        (802816U)
#define RUHMI_COMMAND_STREAM_BYTES      (24520U)

extern uint8_t       sub_0004_model_data[RUHMI_MODEL_DATA_BYTES];
extern uint8_t       sub_0004_arena[RUHMI_MODEL_ARENA_BYTES];
extern uint8_t       sub_0004_command_stream[RUHMI_COMMAND_STREAM_BYTES];
extern const uint8_t g_wheat_model_image[RUHMI_MODEL_DATA_BYTES];
extern const uint8_t g_wheat_command_stream_image[RUHMI_COMMAND_STREAM_BYTES];
extern const size_t  sub_0004_model_data_size;
extern const size_t  sub_0004_command_stream_size;

int8_t * GetModelInputPtr_serving_default_images_0(void);
int8_t * GetModelOutputPtr_PartitionedCall_0_70473(void);
int      RunModel(bool clean_outputs);

#endif /* RUHMI_MODEL_API_H_ */

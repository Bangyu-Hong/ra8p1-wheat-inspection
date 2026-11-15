#ifndef WHEAT_AI_BACKEND_H_
#define WHEAT_AI_BACKEND_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "wheat_postprocess.h"

typedef struct st_wheat_ai_backend_info
{
    char const * name;
    char const * model_identity;
    char const * model_hash;
    uint32_t     requests;
    uint32_t     hardware_invocations;
    uint32_t     tiles_completed;
    uint32_t     last_preprocess_us;
    uint32_t     last_inference_us;
    uint32_t     last_postprocess_us;
    uint32_t     source_sanity_output_crc32;
    uint32_t     qualified_board_sanity_output_crc32;
    uint32_t     deterministic_sanity_output_crc32;
    uint32_t     storage_source_crc32;
    uint32_t     storage_runtime_crc32;
    uintptr_t    storage_source_address;
    uintptr_t    storage_runtime_address;
    uint32_t     command_stream_source_crc32;
    uint32_t     command_stream_runtime_crc32;
    uintptr_t    command_stream_source_address;
    uintptr_t    command_stream_runtime_address;
    uint32_t     storage_configured_capacity_bytes;
    app_status_t last_status;
    bool         model_available;
    bool         board_numeric_gate_passed;
    bool         storage_jedec_identity_verified;
    bool         frame_active;
} wheat_ai_backend_info_t;

app_status_t wheat_ai_backend_init(void);
app_status_t wheat_ai_backend_run(uint8_t const * p_input,
                                  uint32_t input_bytes,
                                  wheat_detection_result_t * p_result);
app_status_t wheat_ai_backend_run_frame(uint8_t const * p_source,
                                        uint32_t source_bytes,
                                        uint16_t source_width,
                                        uint16_t source_height,
                                        uint16_t source_stride,
                                        wheat_detection_result_t * p_result);
app_status_t wheat_ai_backend_start_frame(uint8_t const * p_source,
                                          uint32_t source_bytes,
                                          uint16_t source_width,
                                          uint16_t source_height,
                                          uint16_t source_stride);
app_status_t wheat_ai_backend_process_frame_step(wheat_detection_result_t * p_result);
void         wheat_ai_backend_cancel_frame(app_status_t status);
void         wheat_ai_backend_get_info(wheat_ai_backend_info_t * p_info);

#endif /* WHEAT_AI_BACKEND_H_ */

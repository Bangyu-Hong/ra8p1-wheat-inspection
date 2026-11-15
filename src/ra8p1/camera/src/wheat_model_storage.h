#ifndef WHEAT_MODEL_STORAGE_H_
#define WHEAT_MODEL_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

typedef struct st_wheat_model_storage_info
{
    uintptr_t source_address;
    uintptr_t runtime_address;
    uint32_t  bytes;
    uint32_t  expected_crc32;
    uint32_t  source_crc32;
    uint32_t  runtime_crc32;
    uintptr_t command_stream_source_address;
    uintptr_t command_stream_runtime_address;
    uint32_t  command_stream_bytes;
    uint32_t  command_stream_expected_crc32;
    uint32_t  command_stream_source_crc32;
    uint32_t  command_stream_runtime_crc32;
    uint32_t  configured_capacity_bytes;
    int32_t   fsp_status;
    bool      ospi_open;
    bool      copied;
    bool      jedec_identity_verified;
} wheat_model_storage_info_t;

app_status_t wheat_model_storage_init(void);
void         wheat_model_storage_get_info(wheat_model_storage_info_t * p_info);

#endif /* WHEAT_MODEL_STORAGE_H_ */

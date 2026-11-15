#ifndef APP_COMMAND_H_
#define APP_COMMAND_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define APP_COMMAND_CACHE_CAPACITY  (16U)

typedef struct st_app_command_result
{
    uint64_t session_token;
    uint32_t sequence;
    uint16_t response_type;
    uint16_t status_code;
    uint32_t response_crc32;
} app_command_result_t;

typedef struct st_app_command_cache_entry
{
    bool valid;
    app_command_result_t result;
} app_command_cache_entry_t;

typedef struct st_app_command_cache
{
    app_command_cache_entry_t entries[APP_COMMAND_CACHE_CAPACITY];
    uint8_t next_slot;
    uint32_t stores;
    uint32_t duplicates;
    uint32_t evictions;
} app_command_cache_t;

void app_command_cache_init(app_command_cache_t * p_cache);
app_status_t app_command_cache_lookup(app_command_cache_t * p_cache,
                                      uint64_t session_token,
                                      uint32_t sequence,
                                      bool * p_duplicate,
                                      app_command_result_t * p_result);
app_status_t app_command_cache_store(app_command_cache_t * p_cache,
                                     app_command_result_t const * p_result);

#endif /* APP_COMMAND_H_ */

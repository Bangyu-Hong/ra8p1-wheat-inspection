#include "app_command.h"

#include <stddef.h>

void app_command_cache_init (app_command_cache_t * p_cache)
{
    if (NULL != p_cache)
    {
        *p_cache = (app_command_cache_t) {0};
    }
}

app_status_t app_command_cache_lookup (app_command_cache_t * p_cache,
                                       uint64_t session_token,
                                       uint32_t sequence,
                                       bool * p_duplicate,
                                       app_command_result_t * p_result)
{
    uint32_t index;

    if ((NULL == p_cache) || (NULL == p_duplicate) || (NULL == p_result))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_duplicate = false;
    *p_result = (app_command_result_t) {0};
    for (index = 0U; index < APP_COMMAND_CACHE_CAPACITY; index++)
    {
        app_command_cache_entry_t const * p_entry = &p_cache->entries[index];

        if (p_entry->valid && (p_entry->result.session_token == session_token) &&
            (p_entry->result.sequence == sequence))
        {
            *p_duplicate = true;
            *p_result = p_entry->result;
            p_cache->duplicates++;
            break;
        }
    }

    return APP_STATUS_OK;
}

app_status_t app_command_cache_store (app_command_cache_t * p_cache,
                                      app_command_result_t const * p_result)
{
    app_command_cache_entry_t * p_entry;

    if ((NULL == p_cache) || (NULL == p_result))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    p_entry = &p_cache->entries[p_cache->next_slot];
    if (p_entry->valid)
    {
        p_cache->evictions++;
    }
    p_entry->valid  = true;
    p_entry->result = *p_result;
    p_cache->next_slot = (uint8_t) ((p_cache->next_slot + 1U) % APP_COMMAND_CACHE_CAPACITY);
    p_cache->stores++;

    return APP_STATUS_OK;
}

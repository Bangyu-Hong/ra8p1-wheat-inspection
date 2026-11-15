#include "app_telemetry.h"

#include <stddef.h>

app_status_t app_telemetry_init (app_telemetry_store_t * p_store,
                                 app_telemetry_snapshot_t const * p_initial)
{
    if ((NULL == p_store) || (NULL == p_initial))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_store = (app_telemetry_store_t) {0};
    p_store->slots[0] = *p_initial;
    p_store->slots[1] = *p_initial;
    p_store->initialized = true;

    return APP_STATUS_OK;
}

app_status_t app_telemetry_publish (app_telemetry_store_t * p_store,
                                    app_telemetry_snapshot_t const * p_snapshot)
{
    uint8_t next_slot;

    if ((NULL == p_store) || (NULL == p_snapshot))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (!p_store->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    next_slot = (uint8_t) (p_store->active_slot ^ 1U);
    p_store->slots[next_slot] = *p_snapshot;
    p_store->active_slot = next_slot;
    p_store->publishes++;

    return APP_STATUS_OK;
}

app_status_t app_telemetry_get (app_telemetry_store_t * p_store,
                                app_telemetry_snapshot_t * p_snapshot)
{
    if ((NULL == p_store) || (NULL == p_snapshot))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (!p_store->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    *p_snapshot = p_store->slots[p_store->active_slot];
    p_store->reads++;

    return APP_STATUS_OK;
}

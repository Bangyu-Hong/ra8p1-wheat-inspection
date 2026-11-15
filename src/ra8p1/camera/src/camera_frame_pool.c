#include "camera_frame_pool.h"

#include <limits.h>
#include <stddef.h>

typedef struct st_camera_frame_slot
{
    camera_frame_owner_t owner;
    uint32_t             generation;
    uint64_t             acquired_us;
    uint8_t            * p_buffer;
    uint32_t             capacity;
    camera_frame_t       frame;
} camera_frame_slot_t;

static camera_frame_slot_t         g_slots[CAMERA_FRAME_POOL_SLOT_COUNT];
static camera_frame_pool_metrics_t g_metrics;
static uint32_t                    g_next_generation;
static uint32_t                    g_last_sequence;
static bool                        g_initialized;
static bool                        g_stopped;

static bool         camera_frame_pool_is_power_of_two(uint32_t value);
static bool         camera_frame_pool_regions_overlap(uintptr_t first_address,
                                                      uint32_t first_capacity,
                                                      uintptr_t second_address,
                                                      uint32_t second_capacity);
static bool         camera_frame_pool_sequence_after(uint32_t first, uint32_t second);
static app_status_t camera_frame_pool_validate_reservation(camera_frame_reservation_t const * p_reservation,
                                                           camera_frame_owner_t expected_owner);
static void         camera_frame_pool_drop_ready(uint8_t slot);

app_status_t camera_frame_pool_init (uint8_t * const p_buffers[CAMERA_FRAME_POOL_SLOT_COUNT],
                                     uint32_t const capacities[CAMERA_FRAME_POOL_SLOT_COUNT],
                                     uint32_t alignment)
{
    if ((NULL == p_buffers) || (NULL == capacities) ||
        !camera_frame_pool_is_power_of_two(alignment))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    for (uint32_t slot = 0U; slot < CAMERA_FRAME_POOL_SLOT_COUNT; slot++)
    {
        uintptr_t const address = (uintptr_t) p_buffers[slot];

        if ((NULL == p_buffers[slot]) || (0U == capacities[slot]) ||
            (0U != (address & (uintptr_t) (alignment - 1U))) ||
            (0U != (capacities[slot] % alignment)) ||
            (address > (UINTPTR_MAX - capacities[slot])))
        {
            return APP_STATUS_INVALID_ARGUMENT;
        }
    }

    if (camera_frame_pool_regions_overlap((uintptr_t) p_buffers[0], capacities[0],
                                          (uintptr_t) p_buffers[1], capacities[1]))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    g_metrics         = (camera_frame_pool_metrics_t) {0};
    g_last_sequence   = 0U;
    g_stopped         = true;
    for (uint32_t slot = 0U; slot < CAMERA_FRAME_POOL_SLOT_COUNT; slot++)
    {
        g_slots[slot] = (camera_frame_slot_t)
        {
            .owner     = CAMERA_FRAME_FREE,
            .p_buffer  = p_buffers[slot],
            .capacity  = capacities[slot],
            .frame     =
            {
                .capacity = capacities[slot],
                .address  = (uintptr_t) p_buffers[slot],
            },
        };
    }
    g_initialized = true;

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_start (void)
{
    if (!g_initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    g_stopped = false;

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_stop (void)
{
    if (!g_initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    g_stopped = true;

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_reserve (uint8_t excluded_slot,
                                        camera_frame_reservation_t * p_reservation)
{
    uint8_t selected_slot = CAMERA_FRAME_POOL_NO_SLOT;

    if (NULL == p_reservation)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (!g_initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    if (g_stopped)
    {
        return APP_STATUS_BUSY;
    }

    for (uint8_t slot = 0U; slot < CAMERA_FRAME_POOL_SLOT_COUNT; slot++)
    {
        if ((slot != excluded_slot) && (CAMERA_FRAME_FREE == g_slots[slot].owner))
        {
            selected_slot = slot;
            break;
        }
    }

    if (CAMERA_FRAME_POOL_NO_SLOT == selected_slot)
    {
        for (uint8_t slot = 0U; slot < CAMERA_FRAME_POOL_SLOT_COUNT; slot++)
        {
            if ((slot != excluded_slot) && (CAMERA_FRAME_READY == g_slots[slot].owner))
            {
                if ((CAMERA_FRAME_POOL_NO_SLOT == selected_slot) ||
                    camera_frame_pool_sequence_after(g_slots[selected_slot].frame.sequence,
                                                     g_slots[slot].frame.sequence))
                {
                    selected_slot = slot;
                }
            }
        }
    }

    if (CAMERA_FRAME_POOL_NO_SLOT == selected_slot)
    {
        g_metrics.capture_starved++;
        g_metrics.last_starved_sequence   = g_last_sequence;
        g_metrics.last_starved_generation = g_next_generation;
        return APP_STATUS_BUSY;
    }

    if (CAMERA_FRAME_READY == g_slots[selected_slot].owner)
    {
        camera_frame_pool_drop_ready(selected_slot);
    }

    g_next_generation++;
    if (0U == g_next_generation)
    {
        g_next_generation = 1U;
    }

    g_slots[selected_slot].generation = g_next_generation;
    g_slots[selected_slot].owner      = CAMERA_FRAME_CEU;
    g_slots[selected_slot].frame = (camera_frame_t)
    {
        .generation = g_next_generation,
        .capacity   = g_slots[selected_slot].capacity,
        .address    = (uintptr_t) g_slots[selected_slot].p_buffer,
        .status     = APP_STATUS_BUSY,
    };
    g_metrics.reservations++;

    *p_reservation = (camera_frame_reservation_t)
    {
        .slot       = selected_slot,
        .generation = g_next_generation,
        .p_buffer   = g_slots[selected_slot].p_buffer,
        .capacity   = g_slots[selected_slot].capacity,
    };

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_begin_validation (camera_frame_reservation_t const * p_reservation)
{
    app_status_t const status = camera_frame_pool_validate_reservation(p_reservation, CAMERA_FRAME_CEU);

    if (APP_STATUS_OK != status)
    {
        return status;
    }

    g_slots[p_reservation->slot].owner = CAMERA_FRAME_VALIDATING;

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_publish (camera_frame_reservation_t const * p_reservation,
                                        camera_frame_t const * p_frame)
{
    app_status_t status;

    if (NULL == p_frame)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    status = camera_frame_pool_validate_reservation(p_reservation, CAMERA_FRAME_VALIDATING);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    if ((p_frame->generation != p_reservation->generation) ||
        (p_frame->address != (uintptr_t) p_reservation->p_buffer) ||
        (p_frame->capacity != p_reservation->capacity) ||
        (p_frame->valid_bytes > p_frame->capacity) ||
        (APP_STATUS_OK != p_frame->status))
    {
        g_metrics.invalid_transitions++;
        return APP_STATUS_DATA;
    }

    for (uint8_t slot = 0U; slot < CAMERA_FRAME_POOL_SLOT_COUNT; slot++)
    {
        if ((slot != p_reservation->slot) && (CAMERA_FRAME_READY == g_slots[slot].owner))
        {
            camera_frame_pool_drop_ready(slot);
        }
    }

    g_slots[p_reservation->slot].frame = *p_frame;
    g_slots[p_reservation->slot].owner = CAMERA_FRAME_READY;
    g_last_sequence = p_frame->sequence;
    g_metrics.published++;

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_quarantine (camera_frame_reservation_t const * p_reservation,
                                           app_status_t status)
{
    if ((NULL == p_reservation) || (APP_STATUS_OK == status))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (!g_initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    if ((p_reservation->slot >= CAMERA_FRAME_POOL_SLOT_COUNT) ||
        ((CAMERA_FRAME_CEU != g_slots[p_reservation->slot].owner) &&
         (CAMERA_FRAME_VALIDATING != g_slots[p_reservation->slot].owner)) ||
        (p_reservation->generation != g_slots[p_reservation->slot].generation) ||
        (p_reservation->p_buffer != g_slots[p_reservation->slot].p_buffer) ||
        (p_reservation->capacity != g_slots[p_reservation->slot].capacity))
    {
        g_metrics.invalid_transitions++;
        return APP_STATUS_DATA;
    }

    g_slots[p_reservation->slot].owner        = CAMERA_FRAME_QUARANTINED;
    g_slots[p_reservation->slot].frame.status = status;
    g_slots[p_reservation->slot].frame.valid_bytes = 0U;
    g_metrics.quarantined++;

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_discard (camera_frame_reservation_t const * p_reservation,
                                        app_status_t status)
{
    if ((NULL == p_reservation) || (APP_STATUS_OK == status))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (!g_initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    if ((p_reservation->slot >= CAMERA_FRAME_POOL_SLOT_COUNT) ||
        ((CAMERA_FRAME_CEU != g_slots[p_reservation->slot].owner) &&
         (CAMERA_FRAME_VALIDATING != g_slots[p_reservation->slot].owner)) ||
        (p_reservation->generation != g_slots[p_reservation->slot].generation) ||
        (p_reservation->p_buffer != g_slots[p_reservation->slot].p_buffer) ||
        (p_reservation->capacity != g_slots[p_reservation->slot].capacity))
    {
        g_metrics.invalid_transitions++;
        return APP_STATUS_DATA;
    }

    g_slots[p_reservation->slot].frame.status = status;
    g_slots[p_reservation->slot].frame.valid_bytes = 0U;
    g_slots[p_reservation->slot].owner = CAMERA_FRAME_FREE;
    g_metrics.discarded_errors++;

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_acquire_latest (uint64_t now_us, camera_frame_lease_t * p_lease)
{
    uint8_t selected_slot = CAMERA_FRAME_POOL_NO_SLOT;

    if (NULL == p_lease)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (!g_initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    for (uint8_t slot = 0U; slot < CAMERA_FRAME_POOL_SLOT_COUNT; slot++)
    {
        if (CAMERA_FRAME_CONSUMER == g_slots[slot].owner)
        {
            return APP_STATUS_BUSY;
        }

        if ((CAMERA_FRAME_READY == g_slots[slot].owner) &&
            ((CAMERA_FRAME_POOL_NO_SLOT == selected_slot) ||
             camera_frame_pool_sequence_after(g_slots[slot].frame.sequence,
                                              g_slots[selected_slot].frame.sequence)))
        {
            selected_slot = slot;
        }
    }

    if (CAMERA_FRAME_POOL_NO_SLOT == selected_slot)
    {
        return APP_STATUS_BUSY;
    }

    g_slots[selected_slot].owner       = CAMERA_FRAME_CONSUMER;
    g_slots[selected_slot].acquired_us = now_us;
    g_metrics.acquired++;
    *p_lease = (camera_frame_lease_t)
    {
        .slot       = selected_slot,
        .generation = g_slots[selected_slot].generation,
        .p_frame    = &g_slots[selected_slot].frame,
    };

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_validate_lease (camera_frame_lease_t const * p_lease)
{
    if (NULL == p_lease)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (!g_initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    if ((p_lease->slot >= CAMERA_FRAME_POOL_SLOT_COUNT) ||
        (CAMERA_FRAME_CONSUMER != g_slots[p_lease->slot].owner) ||
        (p_lease->generation != g_slots[p_lease->slot].generation) ||
        (p_lease->p_frame != &g_slots[p_lease->slot].frame))
    {
        return APP_STATUS_DATA;
    }

    return APP_STATUS_OK;
}

app_status_t camera_frame_pool_release (uint64_t now_us, camera_frame_lease_t const * p_lease)
{
    app_status_t status;
    uint64_t     hold_us;

    status = camera_frame_pool_validate_lease(p_lease);
    if (APP_STATUS_OK != status)
    {
        if ((APP_STATUS_INVALID_ARGUMENT != status) && (APP_STATUS_NOT_INITIALIZED != status))
        {
            g_metrics.stale_releases++;
        }
        return status;
    }

    hold_us = now_us - g_slots[p_lease->slot].acquired_us;
    g_metrics.last_consumer_hold_us = hold_us;
    if (hold_us > g_metrics.max_consumer_hold_us)
    {
        g_metrics.max_consumer_hold_us = hold_us;
    }

    g_slots[p_lease->slot].owner = CAMERA_FRAME_FREE;
    g_metrics.released++;

    return APP_STATUS_OK;
}

void camera_frame_pool_get_snapshot (camera_frame_pool_snapshot_t * p_snapshot)
{
    if (NULL != p_snapshot)
    {
        *p_snapshot = (camera_frame_pool_snapshot_t)
        {
            .metrics     = g_metrics,
            .initialized = g_initialized,
            .stopped     = g_stopped,
        };

        for (uint32_t slot = 0U; slot < CAMERA_FRAME_POOL_SLOT_COUNT; slot++)
        {
            p_snapshot->owner[slot]      = g_slots[slot].owner;
            p_snapshot->generation[slot] = g_slots[slot].generation;
            p_snapshot->sequence[slot]   = g_slots[slot].frame.sequence;
        }
    }
}

#if defined(CAMERA_FRAME_POOL_TEST)
void camera_frame_pool_test_set_next_generation (uint32_t generation)
{
    g_next_generation = generation;
}
#endif

static bool camera_frame_pool_is_power_of_two (uint32_t value)
{
    return (0U != value) && (0U == (value & (value - 1U)));
}

static bool camera_frame_pool_regions_overlap (uintptr_t first_address,
                                               uint32_t first_capacity,
                                               uintptr_t second_address,
                                               uint32_t second_capacity)
{
    uintptr_t const first_end  = first_address + first_capacity;
    uintptr_t const second_end = second_address + second_capacity;

    return (first_address < second_end) && (second_address < first_end);
}

static bool camera_frame_pool_sequence_after (uint32_t first, uint32_t second)
{
    return ((int32_t) (first - second)) > 0;
}

static app_status_t camera_frame_pool_validate_reservation (
    camera_frame_reservation_t const * p_reservation,
    camera_frame_owner_t expected_owner)
{
    if (NULL == p_reservation)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (!g_initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    if ((p_reservation->slot >= CAMERA_FRAME_POOL_SLOT_COUNT) ||
        (expected_owner != g_slots[p_reservation->slot].owner) ||
        (p_reservation->generation != g_slots[p_reservation->slot].generation) ||
        (p_reservation->p_buffer != g_slots[p_reservation->slot].p_buffer) ||
        (p_reservation->capacity != g_slots[p_reservation->slot].capacity))
    {
        g_metrics.invalid_transitions++;
        return APP_STATUS_DATA;
    }

    return APP_STATUS_OK;
}

static void camera_frame_pool_drop_ready (uint8_t slot)
{
    g_metrics.dropped_ready++;
    g_metrics.last_dropped_sequence   = g_slots[slot].frame.sequence;
    g_metrics.last_dropped_generation = g_slots[slot].generation;
    g_slots[slot].owner = CAMERA_FRAME_FREE;
}

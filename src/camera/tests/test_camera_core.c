#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "camera_frame_pool.h"
#include "platform_time.h"

#define TEST_BUFFER_BYTES    (128U)
#define TEST_ALIGNMENT       (32U)
#define TEST_CANARY_BYTES    (32U)

#define TEST_CHECK(expression)                         \
    do                                                 \
    {                                                  \
        if (!(expression))                             \
        {                                              \
            (void) printf("  failed line=%d: %s\n", \
                          __LINE__,                    \
                          #expression);                \
            return false;                              \
        }                                              \
    } while (false)

typedef struct st_test_guarded_buffer
{
    uint8_t prefix[TEST_CANARY_BYTES];
    _Alignas(TEST_ALIGNMENT) uint8_t data[TEST_BUFFER_BYTES];
    uint8_t suffix[TEST_CANARY_BYTES];
} test_guarded_buffer_t;

typedef bool (* test_case_fn_t)(void);

typedef struct st_test_case
{
    char const   * p_name;
    test_case_fn_t run;
} test_case_t;

static test_guarded_buffer_t g_guarded[CAMERA_FRAME_POOL_SLOT_COUNT];

static bool         test_time_zero_and_delta(void);
static bool         test_time_wraps_and_gap(void);
static bool         test_time_runtime_rebase(void);
static bool         test_pool_rejects_invalid_storage(void);
static bool         test_pool_normal_ping_pong(void);
static bool         test_pool_latest_frame_drop(void);
static bool         test_pool_held_lease_and_starvation(void);
static bool         test_pool_stale_duplicate_foreign_release(void);
static bool         test_pool_stale_release_after_reinit(void);
static bool         test_pool_illegal_stop_and_quarantine(void);
static bool         test_pool_recoverable_discard(void);
static bool         test_pool_generation_and_sequence_wrap(void);
static app_status_t test_pool_reset(void);
static app_status_t test_publish(camera_frame_reservation_t const * p_reservation,
                                 uint32_t sequence);
static bool         test_canaries_valid(void);

int main (void)
{
    static test_case_t const cases[] =
    {
        {"time zero/delta", test_time_zero_and_delta},
        {"time wrap/gap", test_time_wraps_and_gap},
        {"time runtime rebase", test_time_runtime_rebase},
        {"pool invalid storage", test_pool_rejects_invalid_storage},
        {"pool normal ping-pong", test_pool_normal_ping_pong},
        {"pool latest-frame drop", test_pool_latest_frame_drop},
        {"pool held lease/starvation", test_pool_held_lease_and_starvation},
        {"pool stale/duplicate/foreign", test_pool_stale_duplicate_foreign_release},
        {"pool stale after reinit", test_pool_stale_release_after_reinit},
        {"pool illegal/stop/quarantine", test_pool_illegal_stop_and_quarantine},
        {"pool recoverable discard", test_pool_recoverable_discard},
        {"pool generation/sequence wrap", test_pool_generation_and_sequence_wrap},
    };
    uint32_t passed = 0U;

    for (uint32_t index = 0U; index < (uint32_t) (sizeof(cases) / sizeof(cases[0])); index++)
    {
        bool const ok = cases[index].run();

        (void) printf("[%s] %s\n", ok ? "pass" : "fail", cases[index].p_name);
        if (!ok)
        {
            (void) printf("summary passed=%u total=%u\n",
                          (unsigned int) passed,
                          (unsigned int) (sizeof(cases) / sizeof(cases[0])));
            return 1;
        }
        passed++;
    }

    (void) printf("summary passed=%u total=%u\n",
                  (unsigned int) passed,
                  (unsigned int) (sizeof(cases) / sizeof(cases[0])));

    return 0;
}

static bool test_time_zero_and_delta (void)
{
    platform_time_counter_t counter;
    uint64_t                now_us = UINT64_MAX;
    uint64_t                recent_us = UINT64_MAX;
    uint32_t                elapsed_us = UINT32_MAX;

    TEST_CHECK(APP_STATUS_OK == platform_time_counter_init(&counter, 1000000U, 1000U, 55U));
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_sample(&counter, 55U, &now_us));
    TEST_CHECK(0U == now_us);
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_sample(&counter, 155U, &now_us));
    TEST_CHECK(100U == now_us);
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_expand_recent(&counter, 105U, &recent_us));
    TEST_CHECK(50U == recent_us);
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_elapsed_us(&counter, 90U, 140U, &elapsed_us));
    TEST_CHECK(50U == elapsed_us);

    return true;
}

static bool test_time_wraps_and_gap (void)
{
    platform_time_counter_t counter;
    uint64_t                now_us;

    TEST_CHECK(APP_STATUS_OK == platform_time_counter_init(&counter,
                                                           1000000U,
                                                           UINT32_MAX,
                                                           UINT32_MAX - 15U));
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_sample(&counter, 16U, &now_us));
    TEST_CHECK(32U == now_us);
    TEST_CHECK(1U == counter.wrap_count);
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_sample(&counter, UINT32_MAX - 15U, &now_us));
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_sample(&counter, 32U, &now_us));
    TEST_CHECK(2U == counter.wrap_count);
    TEST_CHECK(now_us > UINT32_MAX);

    TEST_CHECK(APP_STATUS_OK == platform_time_counter_init(&counter, 1000000U, 100U, 0U));
    TEST_CHECK(APP_STATUS_TIMEOUT == platform_time_counter_sample(&counter, 101U, &now_us));
    TEST_CHECK(101U == now_us);
    TEST_CHECK(!counter.healthy);
    TEST_CHECK(1U == counter.service_gap_errors);
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_sample(&counter, 102U, &now_us));
    TEST_CHECK(102U == now_us);
    TEST_CHECK(counter.healthy);
    TEST_CHECK(1U == counter.service_gap_errors);

    return true;
}

static bool test_time_runtime_rebase (void)
{
    platform_time_counter_t counter = {0};
    uint64_t                now_us = UINT64_MAX;

    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == platform_time_counter_rebase(NULL, 0U));
    TEST_CHECK(APP_STATUS_NOT_INITIALIZED == platform_time_counter_rebase(&counter, 0U));
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_init(&counter, 1000000U, 100U, 10U));
    TEST_CHECK(APP_STATUS_TIMEOUT == platform_time_counter_sample(&counter, 111U, &now_us));
    TEST_CHECK(!counter.healthy);
    TEST_CHECK(1U == counter.service_gap_errors);

    TEST_CHECK(APP_STATUS_OK == platform_time_counter_rebase(&counter, 5000U));
    TEST_CHECK(counter.healthy);
    TEST_CHECK(0U == counter.total_cycles);
    TEST_CHECK(0U == counter.service_calls);
    TEST_CHECK(0U == counter.wrap_count);
    TEST_CHECK(0U == counter.service_gap_errors);
    TEST_CHECK(5000U == counter.last_raw_cycles);
    TEST_CHECK(APP_STATUS_OK == platform_time_counter_sample(&counter, 5050U, &now_us));
    TEST_CHECK(50U == now_us);

    return true;
}

static bool test_pool_normal_ping_pong (void)
{
    camera_frame_reservation_t first;
    camera_frame_reservation_t second;
    camera_frame_lease_t       lease;
    camera_frame_pool_snapshot_t snapshot;

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &first));
    TEST_CHECK(APP_STATUS_OK == test_publish(&first, 1U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_acquire_latest(100U, &lease));
    TEST_CHECK(first.slot == lease.slot);
    TEST_CHECK(first.generation == lease.generation);
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_release(150U, &lease));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(first.slot, &second));
    TEST_CHECK(first.slot != second.slot);
    TEST_CHECK(APP_STATUS_OK == test_publish(&second, 2U));
    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(2U == snapshot.metrics.reservations);
    TEST_CHECK(2U == snapshot.metrics.published);
    TEST_CHECK(1U == snapshot.metrics.released);
    TEST_CHECK(50U == snapshot.metrics.last_consumer_hold_us);
    TEST_CHECK(test_canaries_valid());

    return true;
}

static bool test_pool_rejects_invalid_storage (void)
{
    uint8_t * buffers[CAMERA_FRAME_POOL_SLOT_COUNT] =
    {
        g_guarded[0].data,
        g_guarded[1].data,
    };
    uint32_t capacities[CAMERA_FRAME_POOL_SLOT_COUNT] =
    {
        TEST_BUFFER_BYTES,
        TEST_BUFFER_BYTES,
    };

    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == camera_frame_pool_init(buffers, capacities, 0U));
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == camera_frame_pool_init(buffers, capacities, 24U));

    buffers[0]++;
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == camera_frame_pool_init(buffers, capacities, TEST_ALIGNMENT));
    buffers[0]--;

    capacities[0]--;
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == camera_frame_pool_init(buffers, capacities, TEST_ALIGNMENT));
    capacities[0]++;

    buffers[1] = buffers[0];
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == camera_frame_pool_init(buffers, capacities, TEST_ALIGNMENT));
    TEST_CHECK(APP_STATUS_OK == test_pool_reset());

    return true;
}

static bool test_pool_latest_frame_drop (void)
{
    camera_frame_reservation_t first;
    camera_frame_reservation_t second;
    camera_frame_lease_t       lease;
    camera_frame_pool_snapshot_t snapshot;

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &first));
    TEST_CHECK(APP_STATUS_OK == test_publish(&first, 1U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(first.slot, &second));
    TEST_CHECK(APP_STATUS_OK == test_publish(&second, 2U));
    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(1U == snapshot.metrics.dropped_ready);
    TEST_CHECK(1U == snapshot.metrics.last_dropped_sequence);
    TEST_CHECK(CAMERA_FRAME_FREE == snapshot.owner[first.slot]);
    TEST_CHECK(CAMERA_FRAME_READY == snapshot.owner[second.slot]);
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_acquire_latest(0U, &lease));
    TEST_CHECK(2U == lease.p_frame->sequence);

    return true;
}

static bool test_pool_held_lease_and_starvation (void)
{
    camera_frame_reservation_t first;
    camera_frame_reservation_t second;
    camera_frame_reservation_t replacement;
    camera_frame_reservation_t recycled;
    camera_frame_lease_t       held;
    camera_frame_pool_snapshot_t snapshot;
    uint8_t                    held_copy[TEST_BUFFER_BYTES];

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    (void) memset(g_guarded[0].data, 0x5AU, sizeof(g_guarded[0].data));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &first));
    TEST_CHECK(APP_STATUS_OK == test_publish(&first, 10U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_acquire_latest(100U, &held));
    (void) memcpy(held_copy, (void const *) held.p_frame->address, sizeof(held_copy));

    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(first.slot, &second));
    TEST_CHECK(APP_STATUS_OK == test_publish(&second, 11U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(first.slot, &replacement));
    TEST_CHECK(second.slot == replacement.slot);
    TEST_CHECK(second.generation != replacement.generation);
    (void) memset(replacement.p_buffer, 0xC3, replacement.capacity);
    TEST_CHECK(0 == memcmp(held_copy, (void const *) held.p_frame->address, sizeof(held_copy)));
    TEST_CHECK(APP_STATUS_BUSY == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &second));
    TEST_CHECK(APP_STATUS_OK == test_publish(&replacement, 12U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &recycled));
    TEST_CHECK(replacement.slot == recycled.slot);
    TEST_CHECK(replacement.generation != recycled.generation);
    TEST_CHECK(0 == memcmp(held_copy, (void const *) held.p_frame->address, sizeof(held_copy)));

    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(CAMERA_FRAME_CONSUMER == snapshot.owner[first.slot]);
    TEST_CHECK(CAMERA_FRAME_CEU == snapshot.owner[recycled.slot]);
    TEST_CHECK(2U == snapshot.metrics.dropped_ready);
    TEST_CHECK(1U == snapshot.metrics.capture_starved);
    TEST_CHECK(test_canaries_valid());

    return true;
}

static bool test_pool_stale_duplicate_foreign_release (void)
{
    camera_frame_reservation_t reservation;
    camera_frame_lease_t       lease;
    camera_frame_lease_t       stale;
    camera_frame_lease_t       foreign;
    camera_frame_t             foreign_frame = {0};
    camera_frame_pool_snapshot_t snapshot;

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &reservation));
    TEST_CHECK(APP_STATUS_OK == test_publish(&reservation, 1U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_acquire_latest(10U, &lease));
    stale = lease;
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_release(20U, &lease));
    TEST_CHECK(APP_STATUS_DATA == camera_frame_pool_release(30U, &stale));

    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &reservation));
    TEST_CHECK(APP_STATUS_OK == test_publish(&reservation, 2U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_acquire_latest(40U, &lease));
    TEST_CHECK(APP_STATUS_DATA == camera_frame_pool_release(50U, &stale));
    foreign = lease;
    foreign.p_frame = &foreign_frame;
    TEST_CHECK(APP_STATUS_DATA == camera_frame_pool_release(50U, &foreign));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_release(60U, &lease));

    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(3U == snapshot.metrics.stale_releases);
    TEST_CHECK(2U == snapshot.metrics.released);

    return true;
}

static bool test_pool_stale_release_after_reinit (void)
{
    camera_frame_reservation_t reservation;
    camera_frame_lease_t       stale;
    camera_frame_lease_t       current;
    camera_frame_pool_snapshot_t snapshot;

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &reservation));
    TEST_CHECK(APP_STATUS_OK == test_publish(&reservation, 1U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_acquire_latest(10U, &stale));

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &reservation));
    TEST_CHECK(stale.generation != reservation.generation);
    TEST_CHECK(APP_STATUS_OK == test_publish(&reservation, 2U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_acquire_latest(20U, &current));
    TEST_CHECK(APP_STATUS_DATA == camera_frame_pool_release(30U, &stale));
    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(CAMERA_FRAME_CONSUMER == snapshot.owner[current.slot]);
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_release(40U, &current));

    return true;
}

static bool test_pool_illegal_stop_and_quarantine (void)
{
    camera_frame_reservation_t reservation;
    camera_frame_reservation_t isolated;
    camera_frame_lease_t       lease;
    camera_frame_t             frame = {0};
    camera_frame_pool_snapshot_t snapshot;

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &reservation));
    frame.generation = reservation.generation;
    frame.address    = (uintptr_t) reservation.p_buffer;
    frame.capacity   = reservation.capacity;
    frame.status     = APP_STATUS_OK;
    TEST_CHECK(APP_STATUS_DATA == camera_frame_pool_publish(&reservation, &frame));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_stop());
    TEST_CHECK(APP_STATUS_BUSY == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &reservation));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_quarantine(&reservation, APP_STATUS_IO));
    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(CAMERA_FRAME_QUARANTINED == snapshot.owner[reservation.slot]);
    TEST_CHECK(1U == snapshot.metrics.invalid_transitions);
    TEST_CHECK(1U == snapshot.metrics.quarantined);

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &reservation));
    TEST_CHECK(APP_STATUS_OK == test_publish(&reservation, 20U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(reservation.slot, &isolated));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_quarantine(&isolated, APP_STATUS_IO));
    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(CAMERA_FRAME_READY == snapshot.owner[reservation.slot]);
    TEST_CHECK(CAMERA_FRAME_QUARANTINED == snapshot.owner[isolated.slot]);
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_acquire_latest(0U, &lease));
    TEST_CHECK(20U == lease.p_frame->sequence);

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &isolated));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_begin_validation(&isolated));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_quarantine(&isolated, APP_STATUS_IO));
    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(CAMERA_FRAME_QUARANTINED == snapshot.owner[isolated.slot]);
    TEST_CHECK(0U == snapshot.metrics.invalid_transitions);
    TEST_CHECK(1U == snapshot.metrics.quarantined);

    return true;
}

static bool test_pool_generation_and_sequence_wrap (void)
{
    camera_frame_reservation_t first;
    camera_frame_reservation_t second;
    camera_frame_lease_t       lease;
    camera_frame_pool_snapshot_t snapshot;

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    camera_frame_pool_test_set_next_generation(UINT32_MAX - 1U);
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &first));
    TEST_CHECK(UINT32_MAX == first.generation);
    TEST_CHECK(APP_STATUS_OK == test_publish(&first, UINT32_MAX));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(first.slot, &second));
    TEST_CHECK(1U == second.generation);
    TEST_CHECK(APP_STATUS_OK == test_publish(&second, 1U));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_acquire_latest(0U, &lease));
    TEST_CHECK(1U == lease.generation);
    TEST_CHECK(1U == lease.p_frame->sequence);
    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(UINT32_MAX == snapshot.metrics.last_dropped_sequence);
    TEST_CHECK(UINT32_MAX == snapshot.metrics.last_dropped_generation);

    return true;
}

static bool test_pool_recoverable_discard (void)
{
    camera_frame_reservation_t failed;
    camera_frame_reservation_t replacement;
    camera_frame_pool_snapshot_t snapshot;

    TEST_CHECK(APP_STATUS_OK == test_pool_reset());
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &failed));
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == camera_frame_pool_discard(&failed, APP_STATUS_OK));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_discard(&failed, APP_STATUS_IO));
    TEST_CHECK(APP_STATUS_OK == camera_frame_pool_reserve(CAMERA_FRAME_POOL_NO_SLOT, &replacement));
    TEST_CHECK(failed.slot == replacement.slot);
    TEST_CHECK(failed.generation != replacement.generation);
    TEST_CHECK(APP_STATUS_DATA == camera_frame_pool_discard(&failed, APP_STATUS_IO));
    camera_frame_pool_get_snapshot(&snapshot);
    TEST_CHECK(1U == snapshot.metrics.discarded_errors);
    TEST_CHECK(1U == snapshot.metrics.invalid_transitions);
    TEST_CHECK(CAMERA_FRAME_CEU == snapshot.owner[replacement.slot]);
    TEST_CHECK(test_canaries_valid());

    return true;
}

static app_status_t test_pool_reset (void)
{
    uint8_t * buffers[CAMERA_FRAME_POOL_SLOT_COUNT];
    uint32_t  capacities[CAMERA_FRAME_POOL_SLOT_COUNT];
    app_status_t status;

    for (uint32_t slot = 0U; slot < CAMERA_FRAME_POOL_SLOT_COUNT; slot++)
    {
        (void) memset(g_guarded[slot].prefix, 0xA5, sizeof(g_guarded[slot].prefix));
        (void) memset(g_guarded[slot].data, 0, sizeof(g_guarded[slot].data));
        (void) memset(g_guarded[slot].suffix, 0x5A, sizeof(g_guarded[slot].suffix));
        buffers[slot]    = g_guarded[slot].data;
        capacities[slot] = (uint32_t) sizeof(g_guarded[slot].data);
    }

    status = camera_frame_pool_init(buffers, capacities, TEST_ALIGNMENT);
    if (APP_STATUS_OK == status)
    {
        status = camera_frame_pool_start();
    }

    return status;
}

static app_status_t test_publish (camera_frame_reservation_t const * p_reservation,
                                  uint32_t sequence)
{
    camera_frame_t frame =
    {
        .sequence    = sequence,
        .generation  = p_reservation->generation,
        .width       = 8U,
        .height      = 8U,
        .stride      = 16U,
        .valid_bytes = p_reservation->capacity,
        .capacity    = p_reservation->capacity,
        .address     = (uintptr_t) p_reservation->p_buffer,
        .status      = APP_STATUS_OK,
    };
    app_status_t status = camera_frame_pool_begin_validation(p_reservation);

    if (APP_STATUS_OK == status)
    {
        status = camera_frame_pool_publish(p_reservation, &frame);
    }

    return status;
}

static bool test_canaries_valid (void)
{
    for (uint32_t slot = 0U; slot < CAMERA_FRAME_POOL_SLOT_COUNT; slot++)
    {
        for (uint32_t index = 0U; index < TEST_CANARY_BYTES; index++)
        {
            if ((0xA5U != g_guarded[slot].prefix[index]) ||
                (0x5AU != g_guarded[slot].suffix[index]))
            {
                return false;
            }
        }
    }

    return true;
}

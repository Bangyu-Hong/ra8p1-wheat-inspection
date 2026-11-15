#ifndef CAMERA_FRAME_POOL_H_
#define CAMERA_FRAME_POOL_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define CAMERA_FRAME_POOL_SLOT_COUNT    (2U)
#define CAMERA_FRAME_POOL_NO_SLOT       (UINT8_MAX)

typedef enum e_camera_frame_owner
{
    CAMERA_FRAME_FREE = 0,
    CAMERA_FRAME_CEU,
    CAMERA_FRAME_VALIDATING,
    CAMERA_FRAME_READY,
    CAMERA_FRAME_CONSUMER,
    CAMERA_FRAME_QUARANTINED
} camera_frame_owner_t;

typedef struct st_camera_frame
{
    uint32_t     sequence;
    uint32_t     generation;
    uint32_t     start_cycles;
    uint32_t     end_cycles;
    uint64_t     timestamp_us;
    uint32_t     elapsed_us;
    uint16_t     width;
    uint16_t     height;
    uint16_t     stride;
    uint8_t      pixel_format;
    uint32_t     valid_bytes;
    uint32_t     capacity;
    uintptr_t    address;
    uint32_t     crc32;
    uint32_t     event_flags;
    uint32_t     ceu_cdssr;
    uint32_t     ceu_state;
    app_status_t status;
} camera_frame_t;

typedef struct st_camera_frame_reservation
{
    uint8_t   slot;
    uint32_t  generation;
    uint8_t * p_buffer;
    uint32_t  capacity;
} camera_frame_reservation_t;

typedef struct st_camera_frame_lease
{
    uint8_t                slot;
    uint32_t               generation;
    camera_frame_t const * p_frame;
} camera_frame_lease_t;

typedef struct st_camera_frame_pool_metrics
{
    uint32_t reservations;
    uint32_t published;
    uint32_t acquired;
    uint32_t released;
    uint32_t dropped_ready;
    uint32_t capture_starved;
    uint32_t invalid_transitions;
    uint32_t stale_releases;
    uint32_t quarantined;
    uint32_t discarded_errors;
    uint32_t last_dropped_sequence;
    uint32_t last_dropped_generation;
    uint32_t last_starved_sequence;
    uint32_t last_starved_generation;
    uint64_t last_consumer_hold_us;
    uint64_t max_consumer_hold_us;
} camera_frame_pool_metrics_t;

typedef struct st_camera_frame_pool_snapshot
{
    camera_frame_owner_t       owner[CAMERA_FRAME_POOL_SLOT_COUNT];
    uint32_t                   generation[CAMERA_FRAME_POOL_SLOT_COUNT];
    uint32_t                   sequence[CAMERA_FRAME_POOL_SLOT_COUNT];
    camera_frame_pool_metrics_t metrics;
    bool                       initialized;
    bool                       stopped;
} camera_frame_pool_snapshot_t;

app_status_t camera_frame_pool_init(uint8_t * const p_buffers[CAMERA_FRAME_POOL_SLOT_COUNT],
                                    uint32_t const capacities[CAMERA_FRAME_POOL_SLOT_COUNT],
                                    uint32_t alignment);
app_status_t camera_frame_pool_start(void);
app_status_t camera_frame_pool_stop(void);
app_status_t camera_frame_pool_reserve(uint8_t excluded_slot,
                                       camera_frame_reservation_t * p_reservation);
app_status_t camera_frame_pool_begin_validation(camera_frame_reservation_t const * p_reservation);
app_status_t camera_frame_pool_publish(camera_frame_reservation_t const * p_reservation,
                                       camera_frame_t const * p_frame);
app_status_t camera_frame_pool_quarantine(camera_frame_reservation_t const * p_reservation,
                                          app_status_t status);
app_status_t camera_frame_pool_discard(camera_frame_reservation_t const * p_reservation,
                                       app_status_t status);
app_status_t camera_frame_pool_acquire_latest(uint64_t now_us, camera_frame_lease_t * p_lease);
app_status_t camera_frame_pool_validate_lease(camera_frame_lease_t const * p_lease);
app_status_t camera_frame_pool_release(uint64_t now_us, camera_frame_lease_t const * p_lease);
void         camera_frame_pool_get_snapshot(camera_frame_pool_snapshot_t * p_snapshot);

#if defined(CAMERA_FRAME_POOL_TEST)
void camera_frame_pool_test_set_next_generation(uint32_t generation);
#endif

#endif /* CAMERA_FRAME_POOL_H_ */

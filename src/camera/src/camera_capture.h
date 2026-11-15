#ifndef CAMERA_CAPTURE_H_
#define CAMERA_CAPTURE_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "camera_frame_pool.h"
#include "camera_sensor.h"
#include "r_capture_api.h"

#define CAMERA_FRAME_WIDTH             (640U)
#define CAMERA_FRAME_HEIGHT            (480U)
#define CAMERA_FRAME_BYTES_PER_PIXEL   (2U)
#define CAMERA_FRAME_STRIDE            (1280U)
#define CAMERA_FRAME_BYTES             (614400U)
#define CAMERA_FRAME_ALIGNMENT         (32U)
#define CAMERA_CAPTURE_TIMEOUT_US      (250000U)

typedef enum e_camera_capture_state
{
    CAMERA_CAPTURE_STATE_CLOSED = 0,
    CAMERA_CAPTURE_STATE_IDLE,
    CAMERA_CAPTURE_STATE_STREAMING,
    CAMERA_CAPTURE_STATE_STOP_PENDING,
    CAMERA_CAPTURE_STATE_STOPPED,
    CAMERA_CAPTURE_STATE_ERROR
} camera_capture_state_t;

typedef struct st_camera_capture_diagnostics
{
    uint32_t               requests;
    uint32_t               completed_frames;
    uint32_t               valid_frames;
    uint32_t               published_frames;
    uint32_t               acquired_frames;
    uint32_t               released_frames;
    uint32_t               dropped_ready_frames;
    uint32_t               capture_starved;
    uint32_t               callback_count;
    uint32_t               interrupt_mask;
    uint32_t               stream_start_count;
    uint32_t               stream_resume_count;
    uint32_t               capture_start_count;
    uint32_t               recoverable_errors;
    uint32_t               recovery_rearms;
    uint32_t               recovery_confirmed;
    uint32_t               recovery_failures;
    uint32_t               consecutive_recoverable_errors;
    uint32_t               max_consecutive_recoverable_errors;
    uint32_t               timeouts;
    uint32_t               errors;
    uint32_t               invalid_transitions;
    uint32_t               stale_releases;
    uint32_t               last_event_flags;
    uint32_t               last_error_flags;
    uint32_t               last_cdssr;
    uint32_t               last_capsr;
    uint32_t               last_cstsr;
    uint32_t               last_cetcr;
    uint32_t               last_ceier;
    uint32_t               last_sequence;
    uint32_t               last_crc32;
    uint32_t               crc_calculations;
    uint32_t               crc_errors;
    uint32_t               crc_self_test_failures;
    uint32_t               last_generation;
    uint32_t               last_dropped_sequence;
    uint32_t               last_dropped_generation;
    uint32_t               last_starved_sequence;
    uint32_t               last_starved_generation;
    uint8_t                last_slot;
    uintptr_t              last_callback_buffer;
    uint64_t               last_completion_us;
    uint64_t               last_frame_interval_us;
    uint64_t               min_frame_interval_us;
    uint64_t               max_frame_interval_us;
    uint64_t               last_consumer_hold_us;
    uint64_t               max_consumer_hold_us;
    uint32_t               last_rearm_us;
    uint32_t               max_rearm_us;
    uint32_t               last_process_us;
    uint32_t               max_process_us;
    fsp_err_t              last_fsp_error;
    fsp_err_t              last_close_error;
    int32_t                last_crc_driver_error;
    app_status_t           last_crc_error_status;
    app_status_t           last_status;
    camera_capture_state_t state;
    bool                   initialized;
    bool                   capture_active;
    bool                   stop_requested;
    bool                   crc_initialized;
} camera_capture_diagnostics_t;

app_status_t camera_capture_open(void);
app_status_t camera_capture_stream_start(void);
app_status_t camera_capture_process(uint64_t now_us);
app_status_t camera_capture_acquire_latest(camera_frame_lease_t * p_lease);
app_status_t camera_capture_release(camera_frame_lease_t const * p_lease);
app_status_t camera_capture_lease_crc32(camera_frame_lease_t const * p_lease, uint32_t * p_crc32);
app_status_t camera_capture_stop_request(void);
app_status_t camera_capture_close(void);
app_status_t camera_capture_single(camera_frame_t * p_frame);
void         camera_capture_get_diagnostics(camera_capture_diagnostics_t * p_diagnostics);
void         camera_ceu_callback(capture_callback_args_t * p_args);

#endif /* CAMERA_CAPTURE_H_ */

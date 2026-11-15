#include "camera_capture.h"

#include <stddef.h>

#include "crc32_ieee.h"
#include "hal_data.h"
#include "platform_time.h"

#if !defined(BSP_CFG_DCACHE_ENABLED)
 #error "BSP_CFG_DCACHE_ENABLED must be defined for the CEU frame ownership contract"
#elif (BSP_CFG_DCACHE_ENABLED != 0)
 #error "CEU cache maintenance policy is required before enabling D-Cache"
#endif

#define CAMERA_CAPTURE_POLL_STEP_US          (10U)
#define CAMERA_CEU_POST_OPEN_DELAY_US        (10U)
#define CAMERA_CEU_CALLBACK_ARGUMENT_ERROR   (0x80000000UL)
#define CAMERA_CRC_OPEN_ERROR                (0x40000000UL)
#define CAMERA_CRC_CALCULATE_ERROR           (0x20000000UL)
#define CAMERA_CRC_SELF_TEST_ERROR           (0x10000000UL)
#define CAMERA_CAPTURE_MAX_CONSECUTIVE_RECOVERIES (3U)
#define CAMERA_CEU_NON_ERROR_EVENTS          ((uint32_t) CEU_EVENT_FRAME_END | \
                                              (uint32_t) CEU_EVENT_HD | \
                                              (uint32_t) CEU_EVENT_VD)

#define CAMERA_STATIC_ASSERT(expression, name) \
    typedef char camera_static_assert_ ## name[(expression) ? 1 : -1]

CAMERA_STATIC_ASSERT(CAMERA_FRAME_STRIDE == (CAMERA_FRAME_WIDTH * CAMERA_FRAME_BYTES_PER_PIXEL),
                     stride_matches_width);
CAMERA_STATIC_ASSERT(CAMERA_FRAME_BYTES == (CAMERA_FRAME_STRIDE * CAMERA_FRAME_HEIGHT),
                     bytes_match_geometry);
CAMERA_STATIC_ASSERT(CAMERA_FRAME_WIDTH == CAMERA_SENSOR_VGA_WIDTH,
                     width_matches_sensor_mode);
CAMERA_STATIC_ASSERT(CAMERA_FRAME_HEIGHT == CAMERA_SENSOR_VGA_HEIGHT,
                     height_matches_sensor_mode);
CAMERA_STATIC_ASSERT((CAMERA_FRAME_BYTES % CAMERA_FRAME_ALIGNMENT) == 0U,
                     bytes_are_cache_line_aligned);

static uint8_t g_camera_frame_buffer_0[CAMERA_FRAME_BYTES]
    BSP_ALIGN_VARIABLE(CAMERA_FRAME_ALIGNMENT)
    BSP_PLACE_IN_SECTION(BSP_SECTION_NOINIT);
static uint8_t g_camera_frame_buffer_1[CAMERA_FRAME_BYTES]
    BSP_ALIGN_VARIABLE(CAMERA_FRAME_ALIGNMENT)
    BSP_PLACE_IN_SECTION(BSP_SECTION_NOINIT);

CAMERA_STATIC_ASSERT(sizeof(g_camera_frame_buffer_0) == CAMERA_FRAME_BYTES, buffer_0_capacity);
CAMERA_STATIC_ASSERT(sizeof(g_camera_frame_buffer_1) == CAMERA_FRAME_BYTES, buffer_1_capacity);

typedef struct st_camera_callback_snapshot
{
    uint32_t  event_flags;
    uintptr_t buffer_address;
    uint32_t  end_cycles;
    uint32_t  callback_count;
    bool      done;
} camera_callback_snapshot_t;

static volatile uint32_t  g_callback_event_flags;
static volatile uintptr_t g_callback_buffer_address;
static volatile uint32_t  g_callback_end_cycles;
static volatile uint32_t  g_callback_count;
static volatile bool      g_callback_done;

static camera_frame_reservation_t g_active_reservation;
static uint64_t                    g_active_start_us;
static uint32_t                    g_active_start_cycles;
static uint32_t                    g_next_sequence;
static bool                        g_active;
static bool                        g_ceu_irq_quiesced;
static bool                        g_recovery_pending;
static camera_capture_diagnostics_t g_diagnostics;

static app_status_t camera_capture_validate_configuration(void);
static app_status_t camera_capture_arm(uint8_t excluded_slot, uint64_t now_us);
static app_status_t camera_capture_process_terminal(camera_callback_snapshot_t const * p_callback);
static app_status_t camera_capture_finish_frame(camera_frame_reservation_t const * p_completed,
                                                 camera_callback_snapshot_t const * p_callback,
                                                 capture_status_t const * p_ceu_status,
                                                 uint32_t completed_start_cycles,
                                                 uint32_t elapsed_us,
                                                 uint64_t completion_us,
                                                 app_status_t rearm_status);
static app_status_t camera_capture_recover_vd_error(uint32_t error_flags);
static app_status_t camera_capture_enter_error(app_status_t status,
                                               uint32_t error_flags,
                                               bool quarantine_active);
static app_status_t camera_capture_record_status(app_status_t status);
static void         camera_capture_irq_quiesce(void);
static void         camera_capture_irq_resume(void);
static void         camera_capture_snapshot_registers(void);
static void         camera_capture_reset_callback_state(void);
static void         camera_capture_snapshot_callback(camera_callback_snapshot_t * p_snapshot);
static void         camera_capture_update_crc_diagnostics(void);
static void         camera_capture_update_process_time(uint32_t start_cycles);

app_status_t camera_capture_open (void)
{
    uint8_t * buffers[CAMERA_FRAME_POOL_SLOT_COUNT] =
    {
        g_camera_frame_buffer_0,
        g_camera_frame_buffer_1,
    };
    uint32_t const capacities[CAMERA_FRAME_POOL_SLOT_COUNT] =
    {
        (uint32_t) sizeof(g_camera_frame_buffer_0),
        (uint32_t) sizeof(g_camera_frame_buffer_1),
    };
    platform_time_diagnostics_t time_diagnostics;
    app_status_t                status;
    fsp_err_t                   err;

    if (g_diagnostics.initialized)
    {
        return camera_capture_record_status(APP_STATUS_BUSY);
    }

    g_callback_event_flags    = 0U;
    g_callback_buffer_address = (uintptr_t) 0U;
    g_callback_end_cycles     = 0U;
    g_callback_count          = 0U;
    g_callback_done           = false;
    g_active_reservation      = (camera_frame_reservation_t) {0};
    g_active_start_us         = 0U;
    g_active_start_cycles     = 0U;
    g_next_sequence           = 0U;
    g_active                  = false;
    g_ceu_irq_quiesced        = false;
    g_recovery_pending        = false;
    g_diagnostics             = (camera_capture_diagnostics_t) {0};
    g_diagnostics.state       = CAMERA_CAPTURE_STATE_CLOSED;
    g_diagnostics.last_slot   = CAMERA_FRAME_POOL_NO_SLOT;
    g_diagnostics.last_fsp_error   = FSP_SUCCESS;
    g_diagnostics.last_close_error = FSP_SUCCESS;

    platform_time_get_diagnostics(&time_diagnostics);
    if (!time_diagnostics.initialized)
    {
        g_diagnostics.state = CAMERA_CAPTURE_STATE_ERROR;
        g_diagnostics.errors++;
        return camera_capture_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    status = camera_capture_validate_configuration();
    if (APP_STATUS_OK == status)
    {
        status = camera_frame_pool_init(buffers, capacities, CAMERA_FRAME_ALIGNMENT);
    }
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.state = CAMERA_CAPTURE_STATE_ERROR;
        g_diagnostics.errors++;
        return camera_capture_record_status(status);
    }

    g_diagnostics.interrupt_mask =
        ((ceu_extended_cfg_t const *) g_ceu0_cfg.p_extend)->interrupts_enabled;

    status = crc32_ieee_open();
    camera_capture_update_crc_diagnostics();
    if (APP_STATUS_OK != status)
    {
        uint32_t const error_flag = (0U != g_diagnostics.crc_self_test_failures) ?
                                    CAMERA_CRC_SELF_TEST_ERROR : CAMERA_CRC_OPEN_ERROR;

        return camera_capture_enter_error(status, error_flag, false);
    }

    err = g_ceu0.p_api->open(g_ceu0.p_ctrl, g_ceu0.p_cfg);
    g_diagnostics.last_fsp_error = err;
    if (FSP_SUCCESS != err)
    {
        return camera_capture_enter_error(APP_STATUS_IO, 0U, false);
    }

    g_diagnostics.initialized = true;
    g_diagnostics.state       = CAMERA_CAPTURE_STATE_IDLE;

    /* The hardware requires at least ten external VIO_CLK cycles after Open. */
    R_BSP_SoftwareDelay(CAMERA_CEU_POST_OPEN_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);

    return camera_capture_record_status(APP_STATUS_OK);
}

app_status_t camera_capture_stream_start (void)
{
    bool        const resuming = (CAMERA_CAPTURE_STATE_STOPPED == g_diagnostics.state);
    uint64_t    now_us;
    app_status_t status;

    if (!g_diagnostics.initialized)
    {
        return camera_capture_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    if ((CAMERA_CAPTURE_STATE_IDLE != g_diagnostics.state) &&
        (CAMERA_CAPTURE_STATE_STOPPED != g_diagnostics.state))
    {
        return camera_capture_record_status(APP_STATUS_BUSY);
    }

    status = platform_time_now(&now_us);
    if (APP_STATUS_OK != status)
    {
        return camera_capture_enter_error(status, 0U, false);
    }

    status = camera_frame_pool_start();
    if (APP_STATUS_OK != status)
    {
        return camera_capture_enter_error(status, 0U, false);
    }

    g_diagnostics.stop_requested = false;
    g_diagnostics.state = CAMERA_CAPTURE_STATE_STREAMING;
    status = camera_capture_arm(CAMERA_FRAME_POOL_NO_SLOT, now_us);
    if (APP_STATUS_BUSY == status)
    {
        return camera_capture_record_status(APP_STATUS_BUSY);
    }
    if (APP_STATUS_OK != status)
    {
        return camera_capture_enter_error(status, 0U, false);
    }

    g_diagnostics.stream_start_count++;
    if (resuming)
    {
        g_diagnostics.stream_resume_count++;
    }

    return camera_capture_record_status(APP_STATUS_OK);
}

app_status_t camera_capture_process (uint64_t now_us)
{
    uint32_t const process_start_cycles = platform_time_raw_cycles();
    camera_callback_snapshot_t callback;
    app_status_t status = APP_STATUS_OK;

    if (CAMERA_CAPTURE_STATE_ERROR == g_diagnostics.state)
    {
        return g_diagnostics.last_status;
    }

    if (!g_diagnostics.initialized)
    {
        return camera_capture_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    if ((CAMERA_CAPTURE_STATE_STREAMING != g_diagnostics.state) &&
        (CAMERA_CAPTURE_STATE_STOP_PENDING != g_diagnostics.state))
    {
        camera_capture_update_process_time(process_start_cycles);
        return APP_STATUS_OK;
    }

    if (!g_active)
    {
        if (g_diagnostics.stop_requested)
        {
            (void) camera_frame_pool_stop();
            camera_capture_irq_quiesce();
            g_diagnostics.state = CAMERA_CAPTURE_STATE_STOPPED;
        }
        else
        {
            status = camera_capture_arm(CAMERA_FRAME_POOL_NO_SLOT, now_us);
            if (APP_STATUS_BUSY == status)
            {
                status = APP_STATUS_OK;
            }
            else if (APP_STATUS_OK != status)
            {
                status = camera_capture_enter_error(status, 0U, false);
            }
        }

        camera_capture_update_process_time(process_start_cycles);
        return status;
    }

    camera_capture_snapshot_callback(&callback);
    if (callback.done)
    {
        status = camera_capture_process_terminal(&callback);
    }
    else if ((now_us - g_active_start_us) >= CAMERA_CAPTURE_TIMEOUT_US)
    {
        status = camera_capture_enter_error(APP_STATUS_TIMEOUT, 0U, true);
    }

    camera_capture_update_process_time(process_start_cycles);

    return status;
}

app_status_t camera_capture_acquire_latest (camera_frame_lease_t * p_lease)
{
    uint64_t     now_us;
    app_status_t status;

    if (NULL == p_lease)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    status = platform_time_now(&now_us);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    status = camera_frame_pool_acquire_latest(now_us, p_lease);

    return status;
}

app_status_t camera_capture_release (camera_frame_lease_t const * p_lease)
{
    uint64_t     now_us;
    app_status_t status;

    if (NULL == p_lease)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    status = platform_time_now(&now_us);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    status = camera_frame_pool_release(now_us, p_lease);
    return status;
}

app_status_t camera_capture_lease_crc32 (camera_frame_lease_t const * p_lease, uint32_t * p_crc32)
{
    app_status_t status;

    if ((NULL == p_lease) || (NULL == p_crc32))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    status = camera_frame_pool_validate_lease(p_lease);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    if ((p_lease->p_frame->valid_bytes > p_lease->p_frame->capacity) ||
        (0U == p_lease->p_frame->valid_bytes))
    {
        return APP_STATUS_DATA;
    }

    status = crc32_ieee_calculate((uint8_t const *) p_lease->p_frame->address,
                                  p_lease->p_frame->valid_bytes,
                                  p_crc32);
    camera_capture_update_crc_diagnostics();
    if (APP_STATUS_OK != status)
    {
        return camera_capture_enter_error(status, CAMERA_CRC_CALCULATE_ERROR, g_active);
    }

    return APP_STATUS_OK;
}

app_status_t camera_capture_stop_request (void)
{
    if (!g_diagnostics.initialized)
    {
        return camera_capture_record_status(APP_STATUS_NOT_INITIALIZED);
    }

    if ((CAMERA_CAPTURE_STATE_STOPPED == g_diagnostics.state) ||
        (CAMERA_CAPTURE_STATE_IDLE == g_diagnostics.state))
    {
        (void) camera_frame_pool_stop();
        camera_capture_irq_quiesce();
        g_diagnostics.stop_requested = true;
        g_diagnostics.state = CAMERA_CAPTURE_STATE_STOPPED;
        return camera_capture_record_status(APP_STATUS_OK);
    }

    if ((CAMERA_CAPTURE_STATE_STREAMING != g_diagnostics.state) &&
        (CAMERA_CAPTURE_STATE_STOP_PENDING != g_diagnostics.state))
    {
        return camera_capture_record_status(APP_STATUS_BUSY);
    }

    (void) camera_frame_pool_stop();
    g_diagnostics.stop_requested = true;
    g_diagnostics.state = CAMERA_CAPTURE_STATE_STOP_PENDING;

    return camera_capture_record_status(APP_STATUS_OK);
}

app_status_t camera_capture_close (void)
{
    app_status_t status = APP_STATUS_OK;
    app_status_t crc_status;

    if (g_active)
    {
        (void) camera_frame_pool_quarantine(&g_active_reservation, APP_STATUS_IO);
        g_active = false;
    }
    (void) camera_frame_pool_stop();

    if (g_diagnostics.initialized)
    {
        g_diagnostics.last_close_error = g_ceu0.p_api->close(g_ceu0.p_ctrl);
        if (FSP_SUCCESS != g_diagnostics.last_close_error)
        {
            status = APP_STATUS_IO;
        }
    }

    crc_status = crc32_ieee_close();
    camera_capture_update_crc_diagnostics();
    if ((APP_STATUS_OK == status) && (APP_STATUS_OK != crc_status))
    {
        status = crc_status;
    }

    g_diagnostics.initialized    = false;
    g_diagnostics.capture_active = false;
    g_diagnostics.state          = CAMERA_CAPTURE_STATE_CLOSED;
    g_ceu_irq_quiesced            = false;

    return camera_capture_record_status(status);
}

app_status_t camera_capture_single (camera_frame_t * p_frame)
{
    uint64_t             start_us = 0U;
    uint64_t             now_us = 0U;
    camera_frame_lease_t lease;
    app_status_t         status;

    if (NULL == p_frame)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    status = platform_time_now(&start_us);
    if (APP_STATUS_OK == status)
    {
        status = camera_capture_stream_start();
    }
    if (APP_STATUS_OK == status)
    {
        status = camera_capture_stop_request();
    }

    now_us = start_us;
    while ((APP_STATUS_OK == status) &&
           (CAMERA_CAPTURE_STATE_STOPPED != g_diagnostics.state) &&
           ((now_us - start_us) < CAMERA_CAPTURE_TIMEOUT_US))
    {
        status = platform_time_now(&now_us);
        if (APP_STATUS_OK == status)
        {
            status = camera_capture_process(now_us);
        }
        if ((APP_STATUS_OK == status) &&
            (CAMERA_CAPTURE_STATE_STOPPED != g_diagnostics.state) &&
            ((now_us - start_us) < CAMERA_CAPTURE_TIMEOUT_US))
        {
            uint64_t const remaining_us = CAMERA_CAPTURE_TIMEOUT_US - (now_us - start_us);
            uint32_t const wait_us = (remaining_us < CAMERA_CAPTURE_POLL_STEP_US) ?
                                     (uint32_t) remaining_us :
                                     CAMERA_CAPTURE_POLL_STEP_US;

            R_BSP_SoftwareDelay(wait_us, BSP_DELAY_UNITS_MICROSECONDS);
        }
    }

    if ((APP_STATUS_OK == status) && (CAMERA_CAPTURE_STATE_STOPPED != g_diagnostics.state))
    {
        status = camera_capture_enter_error(APP_STATUS_TIMEOUT, 0U, true);
    }

    if (APP_STATUS_OK == status)
    {
        status = camera_capture_acquire_latest(&lease);
    }
    if (APP_STATUS_OK == status)
    {
        *p_frame = *lease.p_frame;
        status = camera_capture_release(&lease);
    }

    return camera_capture_record_status(status);
}

void camera_capture_get_diagnostics (camera_capture_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        uint32_t const primask = __get_PRIMASK();
        camera_frame_pool_snapshot_t pool;

        camera_capture_update_crc_diagnostics();
        __disable_irq();
        g_diagnostics.callback_count = g_callback_count;
        *p_diagnostics = g_diagnostics;
        if (0U == primask)
        {
            __enable_irq();
        }

        camera_frame_pool_get_snapshot(&pool);
        p_diagnostics->requests            = pool.metrics.reservations;
        p_diagnostics->published_frames    = pool.metrics.published;
        p_diagnostics->acquired_frames     = pool.metrics.acquired;
        p_diagnostics->released_frames     = pool.metrics.released;
        p_diagnostics->dropped_ready_frames = pool.metrics.dropped_ready;
        p_diagnostics->capture_starved     = pool.metrics.capture_starved;
        p_diagnostics->invalid_transitions = pool.metrics.invalid_transitions;
        p_diagnostics->stale_releases      = pool.metrics.stale_releases;
        p_diagnostics->last_consumer_hold_us = pool.metrics.last_consumer_hold_us;
        p_diagnostics->max_consumer_hold_us  = pool.metrics.max_consumer_hold_us;
        p_diagnostics->last_dropped_sequence   = pool.metrics.last_dropped_sequence;
        p_diagnostics->last_dropped_generation = pool.metrics.last_dropped_generation;
        p_diagnostics->last_starved_sequence   = pool.metrics.last_starved_sequence;
        p_diagnostics->last_starved_generation = pool.metrics.last_starved_generation;
    }
}

void camera_ceu_callback (capture_callback_args_t * p_args)
{
    uint32_t event_flags;
    bool     terminal_event;

    if (NULL == p_args)
    {
        event_flags = CAMERA_CEU_CALLBACK_ARGUMENT_ERROR;
        g_callback_event_flags |= event_flags;
        g_callback_count++;

        if (!g_callback_done)
        {
            g_callback_buffer_address = (uintptr_t) 0U;
            g_callback_end_cycles = DWT->CYCCNT;
            __DMB();
            g_callback_done = true;
        }

        return;
    }

    event_flags = p_args->event;
    terminal_event = (0U != (event_flags & (uint32_t) CEU_EVENT_FRAME_END)) ||
                     (0U != (event_flags & ~CAMERA_CEU_NON_ERROR_EVENTS));
    g_callback_event_flags |= event_flags;
    g_callback_count++;

    if (terminal_event && !g_callback_done)
    {
        g_callback_buffer_address = (uintptr_t) p_args->p_buffer;
        g_callback_end_cycles = DWT->CYCCNT;
        __DMB();
        g_callback_done = true;
    }
}

static app_status_t camera_capture_validate_configuration (void)
{
    uintptr_t const first_address  = (uintptr_t) g_camera_frame_buffer_0;
    uintptr_t const second_address = (uintptr_t) g_camera_frame_buffer_1;
    uintptr_t const first_end      = first_address + sizeof(g_camera_frame_buffer_0);
    uintptr_t const second_end     = second_address + sizeof(g_camera_frame_buffer_1);
    ceu_extended_cfg_t const * const p_extend = (ceu_extended_cfg_t const *) g_ceu0_cfg.p_extend;

    if ((NULL == p_extend) ||
        (0U != (first_address & (CAMERA_FRAME_ALIGNMENT - 1U))) ||
        (0U != (second_address & (CAMERA_FRAME_ALIGNMENT - 1U))) ||
        (first_address < second_end && second_address < first_end) ||
        (sizeof(g_camera_frame_buffer_0) != CAMERA_FRAME_BYTES) ||
        (sizeof(g_camera_frame_buffer_1) != CAMERA_FRAME_BYTES) ||
        (CAMERA_FRAME_STRIDE != g_ceu0_cfg.x_capture_pixels) ||
        (CAMERA_FRAME_HEIGHT != g_ceu0_cfg.y_capture_pixels) ||
        (1U != g_ceu0_cfg.bytes_per_pixel) ||
        (0U != p_extend->byte_swapping.swap_8bit_units) ||
        (1U != p_extend->byte_swapping.swap_16bit_units) ||
        (1U != p_extend->byte_swapping.swap_32bit_units) ||
        (0U == (p_extend->interrupts_enabled & R_CEU_CEIER_CPEIE_Msk)) ||
        (0U != (p_extend->interrupts_enabled & R_CEU_CEIER_VDIE_Msk)) ||
        (CAMERA_FRAME_BYTES != p_extend->image_area_size))
    {
        return APP_STATUS_DATA;
    }

    return APP_STATUS_OK;
}

static app_status_t camera_capture_arm (uint8_t excluded_slot, uint64_t now_us)
{
    camera_frame_reservation_t reservation;
    app_status_t               status;
    fsp_err_t                  err;

    status = camera_frame_pool_reserve(excluded_slot, &reservation);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    camera_capture_reset_callback_state();
    g_active_reservation  = reservation;
    g_active_start_us     = now_us;
    g_active_start_cycles = platform_time_raw_cycles();
    g_active              = true;
    g_diagnostics.capture_active = true;

    __DMB();
    err = g_ceu0.p_api->captureStart(g_ceu0.p_ctrl, reservation.p_buffer);
    g_diagnostics.last_fsp_error = err;
    if (FSP_SUCCESS != err)
    {
        (void) camera_frame_pool_quarantine(&reservation, APP_STATUS_IO);
        g_active = false;
        g_diagnostics.capture_active = false;
        return APP_STATUS_IO;
    }

    g_diagnostics.capture_start_count++;
    camera_capture_snapshot_registers();
    camera_capture_irq_resume();

    return APP_STATUS_OK;
}

static app_status_t camera_capture_process_terminal (camera_callback_snapshot_t const * p_callback)
{
    uint32_t const error_flags = p_callback->event_flags & ~CAMERA_CEU_NON_ERROR_EVENTS;
    uintptr_t const expected_address = (uintptr_t) g_active_reservation.p_buffer;
    capture_status_t ceu_status = {0};
    camera_frame_reservation_t completed;
    uint32_t completed_start_cycles;
    uint32_t elapsed_us;
    uint64_t completion_us;
    uint64_t service_now_us;
    app_status_t status;
    app_status_t rearm_status = APP_STATUS_OK;
    fsp_err_t err;

    g_diagnostics.callback_count       = p_callback->callback_count;
    g_diagnostics.last_event_flags     = p_callback->event_flags;
    g_diagnostics.last_callback_buffer = p_callback->buffer_address;
    if (0U != (p_callback->event_flags & (uint32_t) CEU_EVENT_FRAME_END))
    {
        g_diagnostics.completed_frames++;
    }

    if (0U != error_flags)
    {
        if ((uint32_t) CEU_EVENT_VD_ERROR == error_flags)
        {
            return camera_capture_recover_vd_error(error_flags);
        }
        return camera_capture_enter_error(APP_STATUS_IO, error_flags, true);
    }

    if (0U == (p_callback->event_flags & (uint32_t) CEU_EVENT_FRAME_END))
    {
        return camera_capture_enter_error(APP_STATUS_PROTOCOL, p_callback->event_flags, true);
    }

    if (expected_address != p_callback->buffer_address)
    {
        return camera_capture_enter_error(APP_STATUS_DATA, p_callback->event_flags, true);
    }

    err = g_ceu0.p_api->statusGet(g_ceu0.p_ctrl, &ceu_status);
    g_diagnostics.last_fsp_error = err;
    if (FSP_SUCCESS != err)
    {
        return camera_capture_enter_error(APP_STATUS_IO, p_callback->event_flags, true);
    }

    /* CDSSR is not a byte count in the data-synchronous capture mode used here. */
    g_diagnostics.last_cdssr = ceu_status.data_size;
    if ((CAPTURE_STATE_IDLE != ceu_status.state) ||
        (expected_address != (uintptr_t) ceu_status.p_buffer))
    {
        return camera_capture_enter_error(APP_STATUS_DATA, p_callback->event_flags, true);
    }

    status = camera_frame_pool_begin_validation(&g_active_reservation);
    if (APP_STATUS_OK != status)
    {
        return camera_capture_enter_error(status, p_callback->event_flags, true);
    }

    completed = g_active_reservation;
    completed_start_cycles = g_active_start_cycles;
    g_active = false;
    g_diagnostics.capture_active = false;

    status = platform_time_now(&service_now_us);
    if (APP_STATUS_OK == status)
    {
        status = platform_time_expand_recent(p_callback->end_cycles, &completion_us);
    }
    if (APP_STATUS_OK != status)
    {
        (void) camera_frame_pool_quarantine(&completed, status);
        return camera_capture_enter_error(status, p_callback->event_flags, false);
    }

    status = platform_time_elapsed_us(completed_start_cycles, p_callback->end_cycles, &elapsed_us);
    if ((APP_STATUS_OK == status) && (elapsed_us > CAMERA_CAPTURE_TIMEOUT_US))
    {
        status = APP_STATUS_TIMEOUT;
    }
    if (APP_STATUS_OK != status)
    {
        (void) camera_frame_pool_quarantine(&completed, status);
        return camera_capture_enter_error(status, p_callback->event_flags, false);
    }

    if (!g_diagnostics.stop_requested)
    {
        rearm_status = camera_capture_arm(completed.slot, service_now_us);
        if (APP_STATUS_BUSY == rearm_status)
        {
            rearm_status = APP_STATUS_OK;
        }
        else if (APP_STATUS_OK == rearm_status)
        {
            uint32_t rearm_us;

            status = platform_time_elapsed_us(p_callback->end_cycles,
                                              platform_time_raw_cycles(),
                                              &rearm_us);
            if (APP_STATUS_OK == status)
            {
                g_diagnostics.last_rearm_us = rearm_us;
                if (rearm_us > g_diagnostics.max_rearm_us)
                {
                    g_diagnostics.max_rearm_us = rearm_us;
                }
            }
        }
    }

    return camera_capture_finish_frame(&completed,
                                       p_callback,
                                       &ceu_status,
                                       completed_start_cycles,
                                       elapsed_us,
                                       completion_us,
                                       rearm_status);
}

static app_status_t camera_capture_finish_frame (camera_frame_reservation_t const * p_completed,
                                                 camera_callback_snapshot_t const * p_callback,
                                                 capture_status_t const * p_ceu_status,
                                                 uint32_t completed_start_cycles,
                                                 uint32_t elapsed_us,
                                                 uint64_t completion_us,
                                                 app_status_t rearm_status)
{
    camera_frame_t frame;
    uint64_t const previous_completion_us = g_diagnostics.last_completion_us;
    app_status_t   status;

    g_next_sequence++;
    if (0U == g_next_sequence)
    {
        g_next_sequence = 1U;
    }

    frame = (camera_frame_t)
    {
        .sequence      = g_next_sequence,
        .generation    = p_completed->generation,
        .start_cycles  = completed_start_cycles,
        .end_cycles    = p_callback->end_cycles,
        .timestamp_us  = completion_us,
        .elapsed_us    = elapsed_us,
        .width         = CAMERA_FRAME_WIDTH,
        .height        = CAMERA_FRAME_HEIGHT,
        .stride        = CAMERA_FRAME_STRIDE,
        .pixel_format  = (uint8_t) CAMERA_PIXEL_FORMAT_RGB565_BE,
        .valid_bytes   = CAMERA_FRAME_BYTES,
        .capacity      = p_completed->capacity,
        .address       = (uintptr_t) p_completed->p_buffer,
        .event_flags   = p_callback->event_flags,
        .ceu_cdssr     = p_ceu_status->data_size,
        .ceu_state     = (uint32_t) p_ceu_status->state,
        .status        = APP_STATUS_OK,
    };
    status = crc32_ieee_calculate(p_completed->p_buffer, CAMERA_FRAME_BYTES, &frame.crc32);
    camera_capture_update_crc_diagnostics();
    if (APP_STATUS_OK != status)
    {
        (void) camera_frame_pool_quarantine(p_completed, status);
        return camera_capture_enter_error(status, CAMERA_CRC_CALCULATE_ERROR, g_active);
    }

    status = camera_frame_pool_publish(p_completed, &frame);
    if (APP_STATUS_OK != status)
    {
        (void) camera_frame_pool_quarantine(p_completed, status);
        return camera_capture_enter_error(status, p_callback->event_flags, g_active);
    }

    g_diagnostics.valid_frames++;
    g_diagnostics.last_sequence      = frame.sequence;
    g_diagnostics.last_crc32         = frame.crc32;
    g_diagnostics.last_generation    = frame.generation;
    g_diagnostics.last_slot          = p_completed->slot;
    g_diagnostics.last_completion_us = completion_us;
    if (0U != previous_completion_us)
    {
        uint64_t const interval_us = completion_us - previous_completion_us;

        g_diagnostics.last_frame_interval_us = interval_us;
        if ((0U == g_diagnostics.min_frame_interval_us) ||
            (interval_us < g_diagnostics.min_frame_interval_us))
        {
            g_diagnostics.min_frame_interval_us = interval_us;
        }
        if (interval_us > g_diagnostics.max_frame_interval_us)
        {
            g_diagnostics.max_frame_interval_us = interval_us;
        }
    }
    if (g_diagnostics.stop_requested)
    {
        (void) camera_frame_pool_stop();
        camera_capture_irq_quiesce();
        g_diagnostics.state = CAMERA_CAPTURE_STATE_STOPPED;
    }

    if (APP_STATUS_OK != rearm_status)
    {
        return camera_capture_enter_error(rearm_status, 0U, false);
    }

    if (g_recovery_pending)
    {
        g_recovery_pending = false;
        g_diagnostics.recovery_confirmed++;
        g_diagnostics.consecutive_recoverable_errors = 0U;
    }

    return camera_capture_record_status(APP_STATUS_OK);
}

static app_status_t camera_capture_recover_vd_error (uint32_t error_flags)
{
    uint64_t now_us;
    app_status_t status;

    camera_capture_snapshot_registers();
    g_diagnostics.last_error_flags = error_flags;
    g_diagnostics.recoverable_errors++;
    g_diagnostics.consecutive_recoverable_errors++;
    if (g_diagnostics.consecutive_recoverable_errors >
        g_diagnostics.max_consecutive_recoverable_errors)
    {
        g_diagnostics.max_consecutive_recoverable_errors =
            g_diagnostics.consecutive_recoverable_errors;
    }

    if (g_diagnostics.consecutive_recoverable_errors >
        CAMERA_CAPTURE_MAX_CONSECUTIVE_RECOVERIES)
    {
        g_diagnostics.recovery_failures++;
        return camera_capture_enter_error(APP_STATUS_IO, error_flags, true);
    }

    status = camera_frame_pool_discard(&g_active_reservation, APP_STATUS_IO);
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.recovery_failures++;
        return camera_capture_enter_error(status, error_flags, true);
    }

    g_active = false;
    g_diagnostics.capture_active = false;
    status = platform_time_now(&now_us);
    if (APP_STATUS_OK == status)
    {
        status = camera_capture_arm(CAMERA_FRAME_POOL_NO_SLOT, now_us);
    }
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.recovery_failures++;
        return camera_capture_enter_error(status, error_flags, g_active);
    }

    g_recovery_pending = true;
    g_diagnostics.recovery_rearms++;
    g_diagnostics.state = CAMERA_CAPTURE_STATE_STREAMING;
    return camera_capture_record_status(APP_STATUS_OK);
}

static app_status_t camera_capture_enter_error (app_status_t status,
                                                uint32_t error_flags,
                                                bool quarantine_active)
{
    if (quarantine_active && g_active)
    {
        (void) camera_frame_pool_quarantine(&g_active_reservation, status);
        g_active = false;
    }

    (void) camera_frame_pool_stop();
    camera_capture_snapshot_registers();
    g_diagnostics.last_error_flags = error_flags;
    g_diagnostics.capture_active   = false;
    g_diagnostics.stop_requested   = true;
    g_diagnostics.state            = CAMERA_CAPTURE_STATE_ERROR;
    if (APP_STATUS_TIMEOUT == status)
    {
        g_diagnostics.timeouts++;
    }
    else
    {
        g_diagnostics.errors++;
    }

    if (g_diagnostics.initialized)
    {
        g_diagnostics.last_close_error = g_ceu0.p_api->close(g_ceu0.p_ctrl);
        g_diagnostics.initialized = false;
    }
    (void) crc32_ieee_close();
    camera_capture_update_crc_diagnostics();
    g_ceu_irq_quiesced = false;
    g_recovery_pending = false;

    return camera_capture_record_status(status);
}

static app_status_t camera_capture_record_status (app_status_t status)
{
    g_diagnostics.last_status = status;

    return status;
}

static void camera_capture_irq_quiesce (void)
{
    if (!g_ceu_irq_quiesced)
    {
        ceu_extended_cfg_t const * const p_extend = (ceu_extended_cfg_t const *) g_ceu0_cfg.p_extend;

        R_BSP_IrqDisable(p_extend->ceu_irq);
        R_BSP_IrqClearPending(p_extend->ceu_irq);
        g_ceu_irq_quiesced = true;
    }
}

static void camera_capture_irq_resume (void)
{
    if (g_ceu_irq_quiesced)
    {
        ceu_extended_cfg_t const * const p_extend = (ceu_extended_cfg_t const *) g_ceu0_cfg.p_extend;

        R_BSP_IrqEnable(p_extend->ceu_irq);
        g_ceu_irq_quiesced = false;
    }
}

static void camera_capture_snapshot_registers (void)
{
    g_diagnostics.last_capsr = R_CEU->CAPSR;
    g_diagnostics.last_cstsr = R_CEU->CSTSR;
    g_diagnostics.last_cetcr = R_CEU->CETCR;
    g_diagnostics.last_ceier = R_CEU->CEIER;
}

static void camera_capture_reset_callback_state (void)
{
    uint32_t const primask = __get_PRIMASK();

    __disable_irq();
    g_callback_event_flags    = 0U;
    g_callback_buffer_address = (uintptr_t) 0U;
    g_callback_end_cycles     = 0U;
    g_callback_done           = false;
    __DMB();
    if (0U == primask)
    {
        __enable_irq();
    }
}

static void camera_capture_snapshot_callback (camera_callback_snapshot_t * p_snapshot)
{
    uint32_t const primask = __get_PRIMASK();

    __disable_irq();
    __DMB();
    p_snapshot->event_flags    = g_callback_event_flags;
    p_snapshot->buffer_address = g_callback_buffer_address;
    p_snapshot->end_cycles     = g_callback_end_cycles;
    p_snapshot->callback_count = g_callback_count;
    p_snapshot->done           = g_callback_done;
    if (0U == primask)
    {
        __enable_irq();
    }
}

static void camera_capture_update_crc_diagnostics (void)
{
    crc32_ieee_diagnostics_t crc;

    crc32_ieee_get_diagnostics(&crc);
    g_diagnostics.crc_calculations       = crc.calculations;
    g_diagnostics.crc_errors             = crc.errors;
    g_diagnostics.crc_self_test_failures = crc.self_test_failures;
    g_diagnostics.last_crc_driver_error  = crc.last_driver_error;
    g_diagnostics.last_crc_error_status  = crc.last_error_status;
    g_diagnostics.crc_initialized        = crc.initialized;
}

static void camera_capture_update_process_time (uint32_t start_cycles)
{
    uint32_t process_us;

    if (APP_STATUS_OK == platform_time_elapsed_us(start_cycles,
                                                  platform_time_raw_cycles(),
                                                  &process_us))
    {
        g_diagnostics.last_process_us = process_us;
        if (process_us > g_diagnostics.max_process_us)
        {
            g_diagnostics.max_process_us = process_us;
        }
    }
}

#include "platform_time.h"

#include <limits.h>
#include <stddef.h>

#if !defined(PLATFORM_TIME_HOST_TEST)
 #include "hal_data.h"
#endif

app_status_t platform_time_counter_init (platform_time_counter_t * p_counter,
                                         uint32_t clock_hz,
                                         uint32_t max_service_us,
                                         uint32_t initial_raw_cycles)
{
    uint32_t cycles_per_us;

    if ((NULL == p_counter) || (0U == max_service_us) ||
        (clock_hz < 1000000U) || (0U != (clock_hz % 1000000U)))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    cycles_per_us = clock_hz / 1000000U;
    if (max_service_us > (UINT32_MAX / cycles_per_us))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_counter = (platform_time_counter_t)
    {
        .clock_hz          = clock_hz,
        .cycles_per_us     = cycles_per_us,
        .max_service_cycles = max_service_us * cycles_per_us,
        .last_raw_cycles   = initial_raw_cycles,
        .initialized       = true,
        .healthy           = true,
    };

    return APP_STATUS_OK;
}

app_status_t platform_time_counter_rebase (platform_time_counter_t * p_counter,
                                           uint32_t initial_raw_cycles)
{
    uint32_t clock_hz;
    uint32_t cycles_per_us;
    uint32_t max_service_cycles;

    if (NULL == p_counter)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (!p_counter->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    clock_hz = p_counter->clock_hz;
    cycles_per_us = p_counter->cycles_per_us;
    max_service_cycles = p_counter->max_service_cycles;
    if ((0U == clock_hz) || (0U == cycles_per_us) || (0U == max_service_cycles))
    {
        return APP_STATUS_DATA;
    }

    *p_counter = (platform_time_counter_t)
    {
        .clock_hz           = clock_hz,
        .cycles_per_us      = cycles_per_us,
        .max_service_cycles = max_service_cycles,
        .last_raw_cycles    = initial_raw_cycles,
        .initialized        = true,
        .healthy            = true,
    };

    return APP_STATUS_OK;
}

app_status_t platform_time_counter_sample (platform_time_counter_t * p_counter,
                                           uint32_t raw_cycles,
                                           uint64_t * p_now_us)
{
    uint32_t     delta_cycles;
    app_status_t status;

    if ((NULL == p_counter) || (NULL == p_now_us))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (!p_counter->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    delta_cycles = raw_cycles - p_counter->last_raw_cycles;
    status = APP_STATUS_OK;
    if (raw_cycles < p_counter->last_raw_cycles)
    {
        p_counter->wrap_count++;
    }

    if (delta_cycles > p_counter->max_service_cycles)
    {
        p_counter->service_gap_errors++;
        p_counter->healthy = false;
        status = APP_STATUS_TIMEOUT;
    }
    else
    {
        p_counter->healthy = true;
    }

    p_counter->total_cycles += delta_cycles;
    p_counter->last_raw_cycles = raw_cycles;
    p_counter->service_calls++;
    *p_now_us = p_counter->total_cycles / p_counter->cycles_per_us;

    return status;
}

app_status_t platform_time_counter_expand_recent (platform_time_counter_t const * p_counter,
                                                  uint32_t raw_cycles,
                                                  uint64_t * p_time_us)
{
    uint32_t age_cycles;

    if ((NULL == p_counter) || (NULL == p_time_us))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (!p_counter->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    age_cycles = p_counter->last_raw_cycles - raw_cycles;
    if ((age_cycles > p_counter->max_service_cycles) ||
        ((uint64_t) age_cycles > p_counter->total_cycles))
    {
        return APP_STATUS_DATA;
    }

    *p_time_us = (p_counter->total_cycles - age_cycles) / p_counter->cycles_per_us;

    return APP_STATUS_OK;
}

app_status_t platform_time_counter_elapsed_us (platform_time_counter_t const * p_counter,
                                               uint32_t start_raw_cycles,
                                               uint32_t end_raw_cycles,
                                               uint32_t * p_elapsed_us)
{
    uint32_t elapsed_cycles;

    if ((NULL == p_counter) || (NULL == p_elapsed_us))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (!p_counter->initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    elapsed_cycles = end_raw_cycles - start_raw_cycles;
    if (elapsed_cycles > p_counter->max_service_cycles)
    {
        return APP_STATUS_TIMEOUT;
    }

    *p_elapsed_us = elapsed_cycles / p_counter->cycles_per_us;

    return APP_STATUS_OK;
}

#if !defined(PLATFORM_TIME_HOST_TEST)

static platform_time_counter_t g_platform_time;

app_status_t platform_time_init (void)
{
    uint32_t start_cycles;
    uint32_t end_cycles;

    if (g_platform_time.initialized)
    {
        return APP_STATUS_OK;
    }

    if ((SystemCoreClock < 1000000U) || (0U != (SystemCoreClock % 1000000U)))
    {
        return APP_STATUS_DATA;
    }

    CoreDebug->DEMCR |= (uint32_t) CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= (uint32_t) DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();

    start_cycles = DWT->CYCCNT;
    for (uint32_t index = 0U; index < 32U; index++)
    {
        __NOP();
    }
    end_cycles = DWT->CYCCNT;
    if (start_cycles == end_cycles)
    {
        return APP_STATUS_IO;
    }

    return platform_time_counter_init(&g_platform_time,
                                      SystemCoreClock,
                                      PLATFORM_TIME_MAX_SERVICE_US,
                                      end_cycles);
}

app_status_t platform_time_now (uint64_t * p_now_us)
{
    return platform_time_counter_sample(&g_platform_time, DWT->CYCCNT, p_now_us);
}

app_status_t platform_time_begin_runtime_epoch (void)
{
    return platform_time_counter_rebase(&g_platform_time, DWT->CYCCNT);
}

app_status_t platform_time_expand_recent (uint32_t raw_cycles, uint64_t * p_time_us)
{
    return platform_time_counter_expand_recent(&g_platform_time, raw_cycles, p_time_us);
}

app_status_t platform_time_elapsed_us (uint32_t start_raw_cycles,
                                       uint32_t end_raw_cycles,
                                       uint32_t * p_elapsed_us)
{
    return platform_time_counter_elapsed_us(&g_platform_time,
                                             start_raw_cycles,
                                             end_raw_cycles,
                                             p_elapsed_us);
}

uint32_t platform_time_raw_cycles (void)
{
    return DWT->CYCCNT;
}

void platform_time_get_diagnostics (platform_time_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        uint64_t const now_us = g_platform_time.initialized ?
                                (g_platform_time.total_cycles / g_platform_time.cycles_per_us) :
                                0U;

        *p_diagnostics = (platform_time_diagnostics_t)
        {
            .total_cycles       = g_platform_time.total_cycles,
            .now_us             = now_us,
            .clock_hz           = g_platform_time.clock_hz,
            .last_raw_cycles    = g_platform_time.last_raw_cycles,
            .service_calls      = g_platform_time.service_calls,
            .wrap_count         = g_platform_time.wrap_count,
            .service_gap_errors = g_platform_time.service_gap_errors,
            .initialized        = g_platform_time.initialized,
            .healthy            = g_platform_time.healthy,
        };
    }
}

#endif

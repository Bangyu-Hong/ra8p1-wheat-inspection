#ifndef PLATFORM_TIME_H_
#define PLATFORM_TIME_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define PLATFORM_TIME_MAX_SERVICE_US    (2000000U)

typedef struct st_platform_time_counter
{
    uint64_t total_cycles;
    uint32_t clock_hz;
    uint32_t cycles_per_us;
    uint32_t max_service_cycles;
    uint32_t last_raw_cycles;
    uint32_t service_calls;
    uint32_t wrap_count;
    uint32_t service_gap_errors;
    bool     initialized;
    bool     healthy;
} platform_time_counter_t;

typedef struct st_platform_time_diagnostics
{
    uint64_t total_cycles;
    uint64_t now_us;
    uint32_t clock_hz;
    uint32_t last_raw_cycles;
    uint32_t service_calls;
    uint32_t wrap_count;
    uint32_t service_gap_errors;
    bool     initialized;
    bool     healthy;
} platform_time_diagnostics_t;

app_status_t platform_time_counter_init(platform_time_counter_t * p_counter,
                                        uint32_t clock_hz,
                                        uint32_t max_service_us,
                                        uint32_t initial_raw_cycles);
app_status_t platform_time_counter_rebase(platform_time_counter_t * p_counter,
                                          uint32_t initial_raw_cycles);
app_status_t platform_time_counter_sample(platform_time_counter_t * p_counter,
                                          uint32_t raw_cycles,
                                          uint64_t * p_now_us);
app_status_t platform_time_counter_expand_recent(platform_time_counter_t const * p_counter,
                                                 uint32_t raw_cycles,
                                                 uint64_t * p_time_us);
app_status_t platform_time_counter_elapsed_us(platform_time_counter_t const * p_counter,
                                              uint32_t start_raw_cycles,
                                              uint32_t end_raw_cycles,
                                              uint32_t * p_elapsed_us);

app_status_t platform_time_init(void);
app_status_t platform_time_begin_runtime_epoch(void);
app_status_t platform_time_now(uint64_t * p_now_us);
app_status_t platform_time_expand_recent(uint32_t raw_cycles, uint64_t * p_time_us);
app_status_t platform_time_elapsed_us(uint32_t start_raw_cycles,
                                      uint32_t end_raw_cycles,
                                      uint32_t * p_elapsed_us);
uint32_t     platform_time_raw_cycles(void);
void         platform_time_get_diagnostics(platform_time_diagnostics_t * p_diagnostics);

#endif /* PLATFORM_TIME_H_ */

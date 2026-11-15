#ifndef SDRAM_TEST_H_
#define SDRAM_TEST_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

typedef enum e_sdram_test_phase
{
    SDRAM_TEST_PHASE_NONE = 0,
    SDRAM_TEST_PHASE_ADDRESS_LINES,
    SDRAM_TEST_PHASE_WALKING_ONES,
    SDRAM_TEST_PHASE_WALKING_ZEROS,
    SDRAM_TEST_PHASE_CHECKERBOARD_A,
    SDRAM_TEST_PHASE_CHECKERBOARD_5,
    SDRAM_TEST_PHASE_DETERMINISTIC_SWEEP,
    SDRAM_TEST_PHASE_COMPLETE
} sdram_test_phase_t;

typedef struct st_sdram_test_result
{
    uintptr_t         first_failure_address;
    uint32_t          first_failure_word;
    uint32_t          expected;
    uint32_t          actual;
    uint32_t          bytes_tested;
    uint64_t          words_verified;
    sdram_test_phase_t phase;
    bool               passed;
} sdram_test_result_t;

typedef app_status_t (* sdram_test_progress_callback_t)(sdram_test_phase_t phase,
                                                        uint32_t completed_words,
                                                        void * p_context);

app_status_t sdram_test_run(volatile uint32_t * p_base,
                            uint32_t bytes,
                            sdram_test_progress_callback_t progress_callback,
                            void * p_context,
                            sdram_test_result_t * p_result);
char const * sdram_test_phase_name(sdram_test_phase_t phase);

#endif /* SDRAM_TEST_H_ */

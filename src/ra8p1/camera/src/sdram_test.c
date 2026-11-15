#include "sdram_test.h"

#include <stddef.h>

#define SDRAM_CHECKERBOARD_A    (0xAAAAAAAAUL)
#define SDRAM_CHECKERBOARD_5    (0x55555555UL)
#define SDRAM_SWEEP_SEED         (0xA5A5A5A5UL)
#define SDRAM_SWEEP_MULTIPLIER   (0x9E3779B9UL)
#define SDRAM_PROGRESS_WORDS     (65536UL)

static app_status_t sdram_test_fail(volatile uint32_t * p_base,
                                    uint32_t word,
                                    uint32_t expected,
                                    uint32_t actual,
                                    sdram_test_phase_t phase,
                                    sdram_test_result_t * p_result);
static app_status_t sdram_test_address_lines(volatile uint32_t * p_base,
                                             uint32_t word_count,
                                             sdram_test_result_t * p_result);
static app_status_t sdram_test_data_lines(volatile uint32_t * p_base,
                                          sdram_test_result_t * p_result);
static app_status_t sdram_test_fill_verify(volatile uint32_t * p_base,
                                           uint32_t word_count,
                                           uint32_t pattern,
                                           sdram_test_phase_t phase,
                                           sdram_test_progress_callback_t progress_callback,
                                           void * p_context,
                                           sdram_test_result_t * p_result);
static app_status_t sdram_test_sweep(volatile uint32_t * p_base,
                                     uint32_t word_count,
                                     sdram_test_progress_callback_t progress_callback,
                                     void * p_context,
                                     sdram_test_result_t * p_result);
static app_status_t sdram_test_progress(sdram_test_progress_callback_t progress_callback,
                                        sdram_test_phase_t phase,
                                        uint32_t completed_words,
                                        void * p_context);
static bool         sdram_test_is_power_of_two(uint32_t value);

app_status_t sdram_test_run (volatile uint32_t * p_base,
                             uint32_t bytes,
                             sdram_test_progress_callback_t progress_callback,
                             void * p_context,
                             sdram_test_result_t * p_result)
{
    uint32_t const word_count = bytes / (uint32_t) sizeof(uint32_t);
    app_status_t status;

    if ((NULL == p_base) || (NULL == p_result) ||
        (0U != ((uintptr_t) p_base & ((uintptr_t) sizeof(uint32_t) - 1U))) ||
        (bytes < (2U * (uint32_t) sizeof(uint32_t))) ||
        (0U != (bytes % (uint32_t) sizeof(uint32_t))) ||
        !sdram_test_is_power_of_two(word_count))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_result = (sdram_test_result_t)
    {
        .bytes_tested = bytes,
        .phase        = SDRAM_TEST_PHASE_NONE,
    };

    status = sdram_test_address_lines(p_base, word_count, p_result);
    if (APP_STATUS_OK == status)
    {
        status = sdram_test_data_lines(p_base, p_result);
    }
    if (APP_STATUS_OK == status)
    {
        status = sdram_test_fill_verify(p_base,
                                        word_count,
                                        SDRAM_CHECKERBOARD_A,
                                        SDRAM_TEST_PHASE_CHECKERBOARD_A,
                                        progress_callback,
                                        p_context,
                                        p_result);
    }
    if (APP_STATUS_OK == status)
    {
        status = sdram_test_fill_verify(p_base,
                                        word_count,
                                        SDRAM_CHECKERBOARD_5,
                                        SDRAM_TEST_PHASE_CHECKERBOARD_5,
                                        progress_callback,
                                        p_context,
                                        p_result);
    }
    if (APP_STATUS_OK == status)
    {
        status = sdram_test_sweep(p_base,
                                  word_count,
                                  progress_callback,
                                  p_context,
                                  p_result);
    }
    if (APP_STATUS_OK == status)
    {
        p_result->phase = SDRAM_TEST_PHASE_COMPLETE;
        p_result->passed = true;
    }

    return status;
}

char const * sdram_test_phase_name (sdram_test_phase_t phase)
{
    static char const * const names[] =
    {
        "none", "address-lines", "walking-ones", "walking-zeros",
        "checkerboard-aa", "checkerboard-55", "deterministic-sweep", "complete",
    };

    if ((uint32_t) phase >= (uint32_t) (sizeof(names) / sizeof(names[0])))
    {
        return "unknown";
    }

    return names[phase];
}

static app_status_t sdram_test_fail (volatile uint32_t * p_base,
                                     uint32_t word,
                                     uint32_t expected,
                                     uint32_t actual,
                                     sdram_test_phase_t phase,
                                     sdram_test_result_t * p_result)
{
    p_result->first_failure_address = (uintptr_t) &p_base[word];
    p_result->first_failure_word = word;
    p_result->expected = expected;
    p_result->actual = actual;
    p_result->phase = phase;
    p_result->passed = false;
    return APP_STATUS_DATA;
}

static app_status_t sdram_test_address_lines (volatile uint32_t * p_base,
                                              uint32_t word_count,
                                              sdram_test_result_t * p_result)
{
    uint32_t const pattern = 0xAAAAAAAAUL;
    uint32_t const inverse = 0x55555555UL;

    p_result->phase = SDRAM_TEST_PHASE_ADDRESS_LINES;
    for (uint32_t offset = 1U; offset < word_count; offset <<= 1U)
    {
        p_base[offset] = pattern;
    }

    p_base[0] = inverse;
    for (uint32_t offset = 1U; offset < word_count; offset <<= 1U)
    {
        uint32_t const actual = p_base[offset];

        p_result->words_verified++;
        if (pattern != actual)
        {
            return sdram_test_fail(p_base,
                                   offset,
                                   pattern,
                                   actual,
                                   SDRAM_TEST_PHASE_ADDRESS_LINES,
                                   p_result);
        }
    }
    p_base[0] = pattern;

    for (uint32_t test_offset = 1U; test_offset < word_count; test_offset <<= 1U)
    {
        p_base[test_offset] = inverse;
        for (uint32_t offset = 1U; offset < word_count; offset <<= 1U)
        {
            if (offset != test_offset)
            {
                uint32_t const actual = p_base[offset];

                p_result->words_verified++;
                if (pattern != actual)
                {
                    return sdram_test_fail(p_base,
                                           offset,
                                           pattern,
                                           actual,
                                           SDRAM_TEST_PHASE_ADDRESS_LINES,
                                           p_result);
                }
            }
        }
        p_base[test_offset] = pattern;
    }

    return APP_STATUS_OK;
}

static app_status_t sdram_test_data_lines (volatile uint32_t * p_base,
                                           sdram_test_result_t * p_result)
{
    for (uint32_t bit = 0U; bit < 32U; bit++)
    {
        uint32_t const pattern = 1UL << bit;
        uint32_t const actual = (p_base[0] = pattern, p_base[0]);

        p_result->phase = SDRAM_TEST_PHASE_WALKING_ONES;
        p_result->words_verified++;
        if (pattern != actual)
        {
            return sdram_test_fail(p_base,
                                   0U,
                                   pattern,
                                   actual,
                                   SDRAM_TEST_PHASE_WALKING_ONES,
                                   p_result);
        }
    }

    for (uint32_t bit = 0U; bit < 32U; bit++)
    {
        uint32_t const pattern = ~(1UL << bit);
        uint32_t const actual = (p_base[0] = pattern, p_base[0]);

        p_result->phase = SDRAM_TEST_PHASE_WALKING_ZEROS;
        p_result->words_verified++;
        if (pattern != actual)
        {
            return sdram_test_fail(p_base,
                                   0U,
                                   pattern,
                                   actual,
                                   SDRAM_TEST_PHASE_WALKING_ZEROS,
                                   p_result);
        }
    }

    return APP_STATUS_OK;
}

static app_status_t sdram_test_fill_verify (volatile uint32_t * p_base,
                                            uint32_t word_count,
                                            uint32_t pattern,
                                            sdram_test_phase_t phase,
                                            sdram_test_progress_callback_t progress_callback,
                                            void * p_context,
                                            sdram_test_result_t * p_result)
{
    p_result->phase = phase;
    for (uint32_t word = 0U; word < word_count; word++)
    {
        p_base[word] = pattern;
        if (0U == ((word + 1U) % SDRAM_PROGRESS_WORDS))
        {
            app_status_t const status = sdram_test_progress(progress_callback,
                                                             phase,
                                                             word + 1U,
                                                             p_context);

            if (APP_STATUS_OK != status)
            {
                return status;
            }
        }
    }
    for (uint32_t word = 0U; word < word_count; word++)
    {
        uint32_t const actual = p_base[word];

        p_result->words_verified++;
        if (pattern != actual)
        {
            return sdram_test_fail(p_base, word, pattern, actual, phase, p_result);
        }
        if (0U == ((word + 1U) % SDRAM_PROGRESS_WORDS))
        {
            app_status_t const status = sdram_test_progress(progress_callback,
                                                             phase,
                                                             word_count + word + 1U,
                                                             p_context);

            if (APP_STATUS_OK != status)
            {
                return status;
            }
        }
    }

    return APP_STATUS_OK;
}

static app_status_t sdram_test_sweep (volatile uint32_t * p_base,
                                      uint32_t word_count,
                                      sdram_test_progress_callback_t progress_callback,
                                      void * p_context,
                                      sdram_test_result_t * p_result)
{
    p_result->phase = SDRAM_TEST_PHASE_DETERMINISTIC_SWEEP;
    for (uint32_t word = 0U; word < word_count; word++)
    {
        p_base[word] = SDRAM_SWEEP_SEED ^ (word * SDRAM_SWEEP_MULTIPLIER);
        if (0U == ((word + 1U) % SDRAM_PROGRESS_WORDS))
        {
            app_status_t const status = sdram_test_progress(progress_callback,
                                                             SDRAM_TEST_PHASE_DETERMINISTIC_SWEEP,
                                                             word + 1U,
                                                             p_context);

            if (APP_STATUS_OK != status)
            {
                return status;
            }
        }
    }
    for (uint32_t word = 0U; word < word_count; word++)
    {
        uint32_t const expected = SDRAM_SWEEP_SEED ^ (word * SDRAM_SWEEP_MULTIPLIER);
        uint32_t const actual = p_base[word];

        p_result->words_verified++;
        if (expected != actual)
        {
            return sdram_test_fail(p_base,
                                   word,
                                   expected,
                                   actual,
                                   SDRAM_TEST_PHASE_DETERMINISTIC_SWEEP,
                                   p_result);
        }
        if (0U == ((word + 1U) % SDRAM_PROGRESS_WORDS))
        {
            app_status_t const status = sdram_test_progress(progress_callback,
                                                             SDRAM_TEST_PHASE_DETERMINISTIC_SWEEP,
                                                             word_count + word + 1U,
                                                             p_context);

            if (APP_STATUS_OK != status)
            {
                return status;
            }
        }
    }

    return APP_STATUS_OK;
}

static app_status_t sdram_test_progress (sdram_test_progress_callback_t progress_callback,
                                         sdram_test_phase_t phase,
                                         uint32_t completed_words,
                                         void * p_context)
{
    return (NULL == progress_callback) ? APP_STATUS_OK :
           progress_callback(phase, completed_words, p_context);
}

static bool sdram_test_is_power_of_two (uint32_t value)
{
    return (0U != value) && (0U == (value & (value - 1U)));
}

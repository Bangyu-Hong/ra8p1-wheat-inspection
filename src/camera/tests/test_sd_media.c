#include <stdint.h>
#include <stdio.h>

#include "sd_media.h"

#define TEST_CHECK(expression)                                                   \
    do                                                                           \
    {                                                                            \
        if (!(expression))                                                       \
        {                                                                        \
            (void) printf("FAIL line=%d expression=%s\n", __LINE__, #expression); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static int test_alignment(void);
static int test_operation_result_priority(void);
static int test_media_event_processing(void);

int main (void)
{
    TEST_CHECK(0 == test_alignment());
    TEST_CHECK(0 == test_operation_result_priority());
    TEST_CHECK(0 == test_media_event_processing());
    (void) printf("sd media host tests passed\n");
    return 0;
}

static int test_alignment (void)
{
    _Alignas(SD_MEDIA_CACHE_LINE_BYTES)
    uint8_t buffer[SD_MEDIA_SECTOR_BYTES + 1U] = {0};

    TEST_CHECK(sd_media_buffer_is_aligned(buffer));
    TEST_CHECK(!sd_media_buffer_is_aligned(&buffer[1]));
    TEST_CHECK(!sd_media_buffer_is_aligned(NULL));
    return 0;
}

static int test_operation_result_priority (void)
{
    TEST_CHECK(APP_STATUS_BUSY ==
               sd_media_evaluate_operation(SD_MEDIA_EVENT_NONE,
                                           SD_MEDIA_TRANSFER_TIMEOUT_US - 1U));
    TEST_CHECK(APP_STATUS_TIMEOUT ==
               sd_media_evaluate_operation(SD_MEDIA_EVENT_NONE,
                                           SD_MEDIA_TRANSFER_TIMEOUT_US));
    TEST_CHECK(APP_STATUS_OK ==
               sd_media_evaluate_operation(SD_MEDIA_EVENT_COMPLETE,
                                           SD_MEDIA_TRANSFER_TIMEOUT_US));
    TEST_CHECK(APP_STATUS_IO ==
               sd_media_evaluate_operation(SD_MEDIA_EVENT_COMPLETE |
                                           SD_MEDIA_EVENT_ERROR,
                                           0U));
    TEST_CHECK(APP_STATUS_UNAVAILABLE ==
               sd_media_evaluate_operation(SD_MEDIA_EVENT_COMPLETE |
                                           SD_MEDIA_EVENT_ERROR |
                                           SD_MEDIA_EVENT_REMOVED,
                                           0U));
    return 0;
}

static int test_media_event_processing (void)
{
    sd_media_diagnostics_t diagnostics;

    sd_media_host_inject_events(SD_MEDIA_EVENT_INSERTED);
    sd_media_process();
    sd_media_get_diagnostics(&diagnostics);
    TEST_CHECK(diagnostics.media_inserted);
    TEST_CHECK(1U == diagnostics.insertions);
    TEST_CHECK(SD_MEDIA_STATE_INITIALIZING == diagnostics.state);

    sd_media_host_inject_events(SD_MEDIA_EVENT_REMOVED);
    sd_media_process();
    sd_media_get_diagnostics(&diagnostics);
    TEST_CHECK(!diagnostics.media_inserted);
    TEST_CHECK(!diagnostics.initialized);
    TEST_CHECK(1U == diagnostics.removals);
    TEST_CHECK(SD_MEDIA_STATE_NO_CARD == diagnostics.state);
    return 0;
}

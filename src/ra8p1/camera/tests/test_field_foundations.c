#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_command.h"
#include "app_telemetry.h"
#include "fault_record.h"
#include "mission_plan.h"
#include "wire_protocol.h"

#define TEST_ASSERT(condition)                                                        \
    do                                                                                \
    {                                                                                 \
        if (!(condition))                                                             \
        {                                                                             \
            (void) fprintf(stderr, "assertion failed: %s (%s:%d)\n",                \
                           #condition, __FILE__, __LINE__);                            \
            return 1;                                                                 \
        }                                                                             \
    } while (false)

typedef struct st_test_frame_sink
{
    uint32_t calls;
    uint16_t last_type;
    uint32_t last_sequence;
    uint32_t last_payload_bytes;
    uint8_t  last_payload[32];
    bool     busy_once;
} test_frame_sink_t;

static app_status_t test_frame_handler(wire_protocol_frame_t const * p_frame,
                                       void * p_context);
static int test_wire_protocol(void);
static int test_mission_plan(void);
static int test_command_cache(void);
static int test_telemetry_store(void);
static int test_fault_queue(void);
static int test_current_firmware_capability_contract(void);

int main (void)
{
    TEST_ASSERT(0 == test_wire_protocol());
    TEST_ASSERT(0 == test_mission_plan());
    TEST_ASSERT(0 == test_command_cache());
    TEST_ASSERT(0 == test_telemetry_store());
    TEST_ASSERT(0 == test_fault_queue());
    TEST_ASSERT(0 == test_current_firmware_capability_contract());

    (void) printf("field-foundations-tests status=ok protocol=ra8p-v1 "
                  "mission=bounded command_cache=16 telemetry=double-slot faults=16\n");
    return 0;
}

static int test_wire_protocol (void)
{
    static uint8_t const hello_payload[] = "{\"client\":\"test\"}";
    static uint8_t const hello_golden[] = {
        0x52U, 0x41U, 0x38U, 0x50U, 0x00U, 0x01U, 0x00U, 0x01U,
        0x01U, 0x02U, 0x03U, 0x04U, 0x00U, 0x00U, 0x00U, 0x11U,
        0x3BU, 0x8FU, 0xE9U, 0xB6U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x7BU, 0x22U, 0x63U, 0x6CU, 0x69U, 0x65U, 0x6EU, 0x74U,
        0x22U, 0x3AU, 0x22U, 0x74U, 0x65U, 0x73U, 0x74U, 0x22U,
        0x7DU
    };
    uint8_t encoded[128];
    uint8_t sticky[256];
    uint8_t decoder_storage[WIRE_PROTOCOL_MAX_FRAME_BYTES];
    uint8_t damaged[sizeof(hello_golden)];
    uint8_t oversized[RA8P_PROTOCOL_HEADER_BYTES] = {
        0x52U, 0x41U, 0x38U, 0x50U, 0x00U, 0x01U, 0x00U, 0x11U,
        0x00U, 0x00U, 0x00U, 0x05U, 0x00U, 0x00U, 0x10U, 0x01U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
    };
    uint32_t encoded_bytes = 0U;
    uint32_t consumed_bytes = 0U;
    wire_protocol_decoder_t decoder;
    wire_protocol_diagnostics_t diagnostics;
    test_frame_sink_t sink = {0};

    TEST_ASSERT(APP_STATUS_OK == wire_protocol_encode(
        RA8P_MESSAGE_HELLO,
        0x01020304UL,
        RA8P_FLAG_NONE,
        hello_payload,
        (uint32_t) (sizeof(hello_payload) - 1U),
        encoded,
        (uint32_t) sizeof(encoded),
        &encoded_bytes));
    TEST_ASSERT(sizeof(hello_golden) == encoded_bytes);
    TEST_ASSERT(0 == memcmp(encoded, hello_golden, sizeof(hello_golden)));
    TEST_ASSERT(0x3B8FE9B6UL == wire_protocol_crc32(
        hello_payload, (uint32_t) (sizeof(hello_payload) - 1U)));

    TEST_ASSERT(APP_STATUS_OK == wire_protocol_decoder_init(
        &decoder, decoder_storage, (uint32_t) sizeof(decoder_storage)));
    TEST_ASSERT(APP_STATUS_OK == wire_protocol_decoder_feed(
        &decoder, encoded, 7U, &consumed_bytes, test_frame_handler, &sink));
    TEST_ASSERT(7U == consumed_bytes);
    TEST_ASSERT(0U == sink.calls);
    TEST_ASSERT(APP_STATUS_OK == wire_protocol_decoder_feed(
        &decoder,
        &encoded[7],
        encoded_bytes - 7U,
        &consumed_bytes,
        test_frame_handler,
        &sink));
    TEST_ASSERT((encoded_bytes - 7U) == consumed_bytes);
    TEST_ASSERT(1U == sink.calls);
    TEST_ASSERT(RA8P_MESSAGE_HELLO == sink.last_type);
    TEST_ASSERT(0x01020304UL == sink.last_sequence);
    TEST_ASSERT((sizeof(hello_payload) - 1U) == sink.last_payload_bytes);
    TEST_ASSERT(0 == memcmp(sink.last_payload,
                            hello_payload,
                            sizeof(hello_payload) - 1U));

    (void) memcpy(sticky, encoded, encoded_bytes);
    (void) memcpy(&sticky[encoded_bytes], encoded, encoded_bytes);
    wire_protocol_decoder_reset(&decoder);
    sink = (test_frame_sink_t) {0};
    TEST_ASSERT(APP_STATUS_OK == wire_protocol_decoder_feed(
        &decoder,
        sticky,
        encoded_bytes * 2U,
        &consumed_bytes,
        test_frame_handler,
        &sink));
    TEST_ASSERT((encoded_bytes * 2U) == consumed_bytes);
    TEST_ASSERT(2U == sink.calls);

    wire_protocol_decoder_reset(&decoder);
    sink = (test_frame_sink_t) {0};
    sink.busy_once = true;
    TEST_ASSERT(APP_STATUS_BUSY == wire_protocol_decoder_feed(
        &decoder, encoded, encoded_bytes, &consumed_bytes, test_frame_handler, &sink));
    TEST_ASSERT(encoded_bytes == consumed_bytes);
    TEST_ASSERT(encoded_bytes == decoder.used);
    TEST_ASSERT(APP_STATUS_OK == wire_protocol_decoder_feed(
        &decoder, NULL, 0U, &consumed_bytes, test_frame_handler, &sink));
    TEST_ASSERT(0U == consumed_bytes);
    TEST_ASSERT(0U == decoder.used);

    (void) memcpy(damaged, hello_golden, sizeof(damaged));
    damaged[sizeof(damaged) - 1U] ^= 1U;
    wire_protocol_decoder_reset(&decoder);
    TEST_ASSERT(APP_STATUS_DATA == wire_protocol_decoder_feed(
        &decoder,
        damaged,
        (uint32_t) sizeof(damaged),
        &consumed_bytes,
        test_frame_handler,
        &sink));

    wire_protocol_decoder_reset(&decoder);
    TEST_ASSERT(APP_STATUS_PROTOCOL == wire_protocol_decoder_feed(
        &decoder,
        oversized,
        (uint32_t) sizeof(oversized),
        &consumed_bytes,
        test_frame_handler,
        &sink));
    wire_protocol_decoder_get_diagnostics(&decoder, &diagnostics);
    TEST_ASSERT(1U == diagnostics.crc_errors);
    TEST_ASSERT(1U == diagnostics.oversized_frames);
    TEST_ASSERT(1U == diagnostics.callback_busy);

    return 0;
}

static int test_mission_plan (void)
{
    mission_plan_v1_t plan;
    mission_validation_t validation;
    uint32_t const all_capabilities = RA8P_CAPABILITY_MISSION_PLAN |
                                      RA8P_CAPABILITY_MOTION |
                                      RA8P_CAPABILITY_SLIDE;

    mission_plan_init_default(&plan);
    TEST_ASSERT(APP_STATUS_OK == mission_plan_validate(&plan,
                                                       all_capabilities,
                                                       &validation));
    TEST_ASSERT(MISSION_ISSUE_NONE == validation.issue_mask);

    plan.rows = 5U;
    plan.columns = 8U;
    TEST_ASSERT(APP_STATUS_UNAVAILABLE == mission_plan_validate(
        &plan, RA8P_CAPABILITY_MISSION_PLAN, &validation));
    TEST_ASSERT(0U != (validation.issue_mask & MISSION_ISSUE_CAPABILITY));
    TEST_ASSERT(0U != (validation.missing_capabilities & RA8P_CAPABILITY_MOTION));

    plan.settle_ms = 40000U;
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == mission_plan_validate(
        &plan, all_capabilities, &validation));
    TEST_ASSERT(0U != (validation.issue_mask & MISSION_ISSUE_SETTLE_TIME));

    mission_plan_init_default(&plan);
    plan.slide_mode = MISSION_SLIDE_SCAN;
    plan.slide_positions_mm[0] = 0;
    plan.slide_positions_mm[1] = 300;
    plan.slide_position_count = 2U;
    plan.slide_scan_distance_mm = 300U;
    plan.slide_shot_interval_mm = 100U;
    TEST_ASSERT(APP_STATUS_OK == mission_plan_validate(
        &plan, all_capabilities, &validation));

    plan.slide_positions_mm[1] = 301;
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == mission_plan_validate(
        &plan, all_capabilities, &validation));
    TEST_ASSERT(0U != (validation.issue_mask & MISSION_ISSUE_SLIDE_POSITIONS));

    plan.slide_positions_mm[1] = 300;
    plan.slide_scan_distance_mm = 301U;
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == mission_plan_validate(
        &plan, all_capabilities, &validation));
    TEST_ASSERT(0U != (validation.issue_mask & MISSION_ISSUE_SLIDE_SCAN_DISTANCE));

    plan.slide_scan_distance_mm = 300U;
    plan.slide_shot_interval_mm = 301U;
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == mission_plan_validate(
        &plan, all_capabilities, &validation));
    TEST_ASSERT(0U != (validation.issue_mask & MISSION_ISSUE_SLIDE_SHOT_INTERVAL));

    return 0;
}

static int test_command_cache (void)
{
    app_command_cache_t cache;
    app_command_result_t stored = {0x0123456789ABCDEFULL, 9U, RA8P_MESSAGE_COMMAND_ACK,
                                   RA8P_STATUS_OK, 0xAABBCCDDUL};
    app_command_result_t found;
    bool duplicate = false;
    uint32_t index;

    app_command_cache_init(&cache);
    TEST_ASSERT(APP_STATUS_OK == app_command_cache_lookup(
        &cache, stored.session_token, stored.sequence, &duplicate, &found));
    TEST_ASSERT(!duplicate);
    TEST_ASSERT(APP_STATUS_OK == app_command_cache_store(&cache, &stored));
    TEST_ASSERT(APP_STATUS_OK == app_command_cache_lookup(
        &cache, stored.session_token, stored.sequence, &duplicate, &found));
    TEST_ASSERT(duplicate);
    TEST_ASSERT(stored.response_crc32 == found.response_crc32);

    for (index = 0U; index < APP_COMMAND_CACHE_CAPACITY; index++)
    {
        stored.sequence = 100U + index;
        TEST_ASSERT(APP_STATUS_OK == app_command_cache_store(&cache, &stored));
    }
    TEST_ASSERT(1U == cache.evictions);

    return 0;
}

static int test_telemetry_store (void)
{
    app_telemetry_store_t store;
    app_telemetry_snapshot_t initial = {0};
    app_telemetry_snapshot_t updated;
    app_telemetry_snapshot_t actual;

    initial.schema_version = 1U;
    initial.sequence = 1U;
    initial.wheat_count = 41;
    TEST_ASSERT(APP_STATUS_OK == app_telemetry_init(&store, &initial));

    updated = initial;
    updated.sequence = 2U;
    updated.wheat_count = 42;
    TEST_ASSERT(APP_STATUS_OK == app_telemetry_publish(&store, &updated));
    TEST_ASSERT(APP_STATUS_OK == app_telemetry_get(&store, &actual));
    TEST_ASSERT(2U == actual.sequence);
    TEST_ASSERT(42 == actual.wheat_count);
    TEST_ASSERT(1U == store.publishes);
    TEST_ASSERT(1U == store.reads);

    return 0;
}

static int test_fault_queue (void)
{
    fault_record_queue_t queue;
    fault_record_t record = {0};
    fault_record_t actual;
    bool available;
    uint32_t index;

    fault_record_queue_init(&queue);
    for (index = 0U; index < (FAULT_RECORD_CAPACITY + 3U); index++)
    {
        record.sequence = index;
        TEST_ASSERT(APP_STATUS_OK == fault_record_push(&queue, &record));
    }
    TEST_ASSERT(FAULT_RECORD_CAPACITY == queue.used);
    TEST_ASSERT(3U == queue.dropped_oldest);
    TEST_ASSERT(APP_STATUS_OK == fault_record_pop(&queue, &actual, &available));
    TEST_ASSERT(available);
    TEST_ASSERT(3U == actual.sequence);

    return 0;
}

static int test_current_firmware_capability_contract (void)
{
    mission_plan_v1_t plan;
    mission_validation_t validation;

    TEST_ASSERT(0U != (APP_TELEMETRY_FOUNDATION_CAPABILITIES &
                       RA8P_CAPABILITY_VISION));
    TEST_ASSERT(0U != (APP_TELEMETRY_FOUNDATION_CAPABILITIES &
                       RA8P_CAPABILITY_AI_INFERENCE));
    TEST_ASSERT(0U != (APP_TELEMETRY_FOUNDATION_CAPABILITIES &
                       RA8P_CAPABILITY_MISSION_PLAN));
    TEST_ASSERT(0U == (APP_TELEMETRY_FOUNDATION_CAPABILITIES &
                       RA8P_CAPABILITY_ENVIRONMENT));
    TEST_ASSERT(0U == (APP_TELEMETRY_FOUNDATION_CAPABILITIES &
                       RA8P_CAPABILITY_SD_STORAGE));
    TEST_ASSERT(0U == (APP_TELEMETRY_FOUNDATION_CAPABILITIES &
                       RA8P_CAPABILITY_LAN_TCP));
    TEST_ASSERT(0U == (APP_TELEMETRY_FOUNDATION_CAPABILITIES &
                       RA8P_CAPABILITY_MOTION));
    TEST_ASSERT(0U == (APP_TELEMETRY_FOUNDATION_CAPABILITIES &
                       RA8P_CAPABILITY_SLIDE));

    mission_plan_init_default(&plan);
    TEST_ASSERT(APP_STATUS_OK == mission_plan_validate(
        &plan, APP_TELEMETRY_FOUNDATION_CAPABILITIES, &validation));

    plan.rows = 2U;
    TEST_ASSERT(APP_STATUS_UNAVAILABLE == mission_plan_validate(
        &plan, APP_TELEMETRY_FOUNDATION_CAPABILITIES, &validation));
    TEST_ASSERT(0U != (validation.missing_capabilities &
                       RA8P_CAPABILITY_MOTION));

    return 0;
}

static app_status_t test_frame_handler (wire_protocol_frame_t const * p_frame,
                                        void * p_context)
{
    test_frame_sink_t * p_sink = (test_frame_sink_t *) p_context;

    if ((NULL == p_frame) || (NULL == p_sink))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (p_sink->busy_once)
    {
        p_sink->busy_once = false;
        return APP_STATUS_BUSY;
    }

    p_sink->calls++;
    p_sink->last_type = p_frame->type;
    p_sink->last_sequence = p_frame->sequence;
    p_sink->last_payload_bytes = p_frame->payload_bytes;
    if (p_frame->payload_bytes > (uint32_t) sizeof(p_sink->last_payload))
    {
        return APP_STATUS_DATA;
    }
    if (0U < p_frame->payload_bytes)
    {
        (void) memcpy(p_sink->last_payload, p_frame->p_payload, p_frame->payload_bytes);
    }

    return APP_STATUS_OK;
}

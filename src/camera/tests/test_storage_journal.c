#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "storage_journal.h"

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

static int test_queue_policy(void);
static int test_record_codec(void);
static int test_config_slots(void);
static int test_argument_guards(void);

int main (void)
{
    TEST_ASSERT(0 == test_queue_policy());
    TEST_ASSERT(0 == test_record_codec());
    TEST_ASSERT(0 == test_config_slots());
    TEST_ASSERT(0 == test_argument_guards());

    (void) printf("storage-journal-tests status=ok queue=8 reject-newest "
                  "record=crc32-config=ab-generation\n");
    return 0;
}

static int test_queue_policy (void)
{
    static uint8_t const payload[] = "{}";
    storage_journal_queue_t queue;
    storage_journal_record_t const * p_record = NULL;
    storage_journal_diagnostics_t diagnostics;
    uint32_t index;

    storage_journal_init(&queue);
    TEST_ASSERT(APP_STATUS_UNAVAILABLE == storage_journal_peek(&queue, &p_record));
    TEST_ASSERT(NULL == p_record);

    for (index = 0U; index < STORAGE_JOURNAL_QUEUE_CAPACITY; index++)
    {
        TEST_ASSERT(APP_STATUS_OK == storage_journal_enqueue(
            &queue,
            (uint16_t) STORAGE_RECORD_TELEMETRY,
            index,
            (uint64_t) index * 1000U,
            payload,
            (uint32_t) (sizeof(payload) - 1U)));
    }
    TEST_ASSERT(APP_STATUS_BUSY == storage_journal_enqueue(
        &queue,
        (uint16_t) STORAGE_RECORD_TELEMETRY,
        99U,
        99000U,
        payload,
        (uint32_t) (sizeof(payload) - 1U)));
    TEST_ASSERT(APP_STATUS_OK == storage_journal_peek(&queue, &p_record));
    TEST_ASSERT(NULL != p_record);
    TEST_ASSERT(0U == p_record->sequence);

    TEST_ASSERT(APP_STATUS_OK == storage_journal_commit(&queue));
    TEST_ASSERT(APP_STATUS_OK == storage_journal_enqueue(
        &queue,
        (uint16_t) STORAGE_RECORD_EVENT,
        99U,
        99000U,
        payload,
        (uint32_t) (sizeof(payload) - 1U)));

    for (index = 1U; index < STORAGE_JOURNAL_QUEUE_CAPACITY; index++)
    {
        TEST_ASSERT(APP_STATUS_OK == storage_journal_peek(&queue, &p_record));
        TEST_ASSERT(index == p_record->sequence);
        TEST_ASSERT(APP_STATUS_OK == storage_journal_commit(&queue));
    }
    TEST_ASSERT(APP_STATUS_OK == storage_journal_peek(&queue, &p_record));
    TEST_ASSERT(99U == p_record->sequence);
    TEST_ASSERT(APP_STATUS_OK == storage_journal_commit(&queue));
    TEST_ASSERT(APP_STATUS_UNAVAILABLE == storage_journal_commit(&queue));

    storage_journal_get_diagnostics(&queue, &diagnostics);
    TEST_ASSERT(9U == diagnostics.enqueued);
    TEST_ASSERT(9U == diagnostics.committed);
    TEST_ASSERT(1U == diagnostics.rejected_full);
    TEST_ASSERT(STORAGE_JOURNAL_QUEUE_CAPACITY == diagnostics.high_water);
    TEST_ASSERT(0U == diagnostics.queued);

    return 0;
}

static int test_record_codec (void)
{
    static uint8_t const payload[] = "{\"v\":1}";
    static uint8_t const golden_header[STORAGE_JOURNAL_HEADER_BYTES] =
    {
        0x52U, 0x38U, 0x4AU, 0x52U, 0x00U, 0x01U, 0x00U, 0x01U,
        0x00U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x07U, 0x5BU, 0xCDU, 0x15U, 0x00U, 0x00U, 0x00U, 0x07U,
        0x84U, 0xDBU, 0xE7U, 0x3DU, 0x5AU, 0x7BU, 0x81U, 0x5AU,
    };
    storage_journal_queue_t queue;
    storage_journal_record_t const * p_record = NULL;
    storage_journal_record_t damaged_record;
    storage_journal_view_t view;
    uint8_t encoded[STORAGE_JOURNAL_HEADER_BYTES + STORAGE_JOURNAL_MAX_PAYLOAD_BYTES];
    uint8_t damaged[sizeof(encoded)];
    uint32_t encoded_bytes = 0U;
    uint32_t record_bytes = 0U;

    storage_journal_init(&queue);
    TEST_ASSERT(APP_STATUS_OK == storage_journal_enqueue(
        &queue,
        (uint16_t) STORAGE_RECORD_TELEMETRY,
        7U,
        123456789ULL,
        payload,
        (uint32_t) (sizeof(payload) - 1U)));
    TEST_ASSERT(APP_STATUS_OK == storage_journal_peek(&queue, &p_record));
    TEST_ASSERT(APP_STATUS_OK == storage_journal_encode(
        p_record,
        encoded,
        (uint32_t) sizeof(encoded),
        &encoded_bytes));
    TEST_ASSERT((STORAGE_JOURNAL_HEADER_BYTES + sizeof(payload) - 1U) == encoded_bytes);
    TEST_ASSERT(0 == memcmp(encoded, golden_header, sizeof(golden_header)));
    TEST_ASSERT(0 == memcmp(&encoded[STORAGE_JOURNAL_HEADER_BYTES],
                            payload,
                            sizeof(payload) - 1U));

    TEST_ASSERT(APP_STATUS_OK == storage_journal_decode(
        encoded, encoded_bytes, &view, &record_bytes));
    TEST_ASSERT(encoded_bytes == record_bytes);
    TEST_ASSERT(STORAGE_JOURNAL_SCHEMA_VERSION == view.schema_version);
    TEST_ASSERT(STORAGE_RECORD_TELEMETRY == view.type);
    TEST_ASSERT(7U == view.sequence);
    TEST_ASSERT(123456789ULL == view.monotonic_us);
    TEST_ASSERT(0x84DBE73DUL == view.payload_crc32);
    TEST_ASSERT((sizeof(payload) - 1U) == view.payload_bytes);
    TEST_ASSERT(0 == memcmp(view.p_payload, payload, sizeof(payload) - 1U));

    TEST_ASSERT(APP_STATUS_BUSY == storage_journal_decode(
        encoded,
        STORAGE_JOURNAL_HEADER_BYTES - 1U,
        &view,
        &record_bytes));
    TEST_ASSERT(APP_STATUS_BUSY == storage_journal_decode(
        encoded,
        encoded_bytes - 1U,
        &view,
        &record_bytes));

    (void) memcpy(damaged, encoded, encoded_bytes);
    damaged[4] ^= 1U;
    TEST_ASSERT(APP_STATUS_PROTOCOL == storage_journal_decode(
        damaged, encoded_bytes, &view, &record_bytes));
    (void) memcpy(damaged, encoded, encoded_bytes);
    damaged[28] ^= 1U;
    TEST_ASSERT(APP_STATUS_DATA == storage_journal_decode(
        damaged, encoded_bytes, &view, &record_bytes));
    (void) memcpy(damaged, encoded, encoded_bytes);
    damaged[STORAGE_JOURNAL_HEADER_BYTES] ^= 1U;
    TEST_ASSERT(APP_STATUS_DATA == storage_journal_decode(
        damaged, encoded_bytes, &view, &record_bytes));

    damaged_record = *p_record;
    damaged_record.payload[0] ^= 1U;
    TEST_ASSERT(APP_STATUS_DATA == storage_journal_encode(
        &damaged_record,
        encoded,
        (uint32_t) sizeof(encoded),
        &encoded_bytes));

    return 0;
}

static int test_config_slots (void)
{
    static uint8_t const payload_a[] = "{\"ssid\":\"lab\"}";
    static uint8_t const payload_b[] = "{\"ssid\":\"field\"}";
    static uint8_t const golden_header[STORAGE_CONFIG_HEADER_BYTES] =
    {
        0x52U, 0x38U, 0x43U, 0x46U, 0x00U, 0x01U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x2AU, 0x00U, 0x00U, 0x00U, 0x0EU,
        0x5FU, 0x8EU, 0xF2U, 0xFAU, 0x94U, 0x08U, 0x15U, 0xA0U,
    };
    uint8_t slot_a[STORAGE_CONFIG_HEADER_BYTES + STORAGE_CONFIG_MAX_PAYLOAD_BYTES];
    uint8_t slot_b[STORAGE_CONFIG_HEADER_BYTES + STORAGE_CONFIG_MAX_PAYLOAD_BYTES];
    uint32_t slot_a_bytes = 0U;
    uint32_t slot_b_bytes = 0U;
    storage_config_view_t selected;

    TEST_ASSERT(APP_STATUS_OK == storage_config_encode(
        42U,
        payload_a,
        (uint32_t) (sizeof(payload_a) - 1U),
        slot_a,
        (uint32_t) sizeof(slot_a),
        &slot_a_bytes));
    TEST_ASSERT(0 == memcmp(slot_a, golden_header, sizeof(golden_header)));
    TEST_ASSERT(APP_STATUS_OK == storage_config_validate(slot_a, slot_a_bytes, &selected));
    TEST_ASSERT(42U == selected.generation);
    TEST_ASSERT(0x5F8EF2FAUL == selected.payload_crc32);

    TEST_ASSERT(APP_STATUS_OK == storage_config_encode(
        43U,
        payload_b,
        (uint32_t) (sizeof(payload_b) - 1U),
        slot_b,
        (uint32_t) sizeof(slot_b),
        &slot_b_bytes));
    TEST_ASSERT(APP_STATUS_OK == storage_config_select(
        slot_a, slot_a_bytes, slot_b, slot_b_bytes, &selected));
    TEST_ASSERT(1U == selected.slot_index);
    TEST_ASSERT(43U == selected.generation);
    TEST_ASSERT(0 == memcmp(selected.p_payload, payload_b, sizeof(payload_b) - 1U));

    slot_b[STORAGE_CONFIG_HEADER_BYTES] ^= 1U;
    TEST_ASSERT(APP_STATUS_OK == storage_config_select(
        slot_a, slot_a_bytes, slot_b, slot_b_bytes, &selected));
    TEST_ASSERT(0U == selected.slot_index);
    TEST_ASSERT(42U == selected.generation);
    slot_a[STORAGE_CONFIG_HEADER_BYTES] ^= 1U;
    TEST_ASSERT(APP_STATUS_DATA == storage_config_select(
        slot_a, slot_a_bytes, slot_b, slot_b_bytes, &selected));

    TEST_ASSERT(APP_STATUS_OK == storage_config_encode(
        UINT32_MAX - 1U,
        payload_a,
        (uint32_t) (sizeof(payload_a) - 1U),
        slot_a,
        (uint32_t) sizeof(slot_a),
        &slot_a_bytes));
    TEST_ASSERT(APP_STATUS_OK == storage_config_encode(
        1U,
        payload_b,
        (uint32_t) (sizeof(payload_b) - 1U),
        slot_b,
        (uint32_t) sizeof(slot_b),
        &slot_b_bytes));
    TEST_ASSERT(APP_STATUS_OK == storage_config_select(
        slot_a, slot_a_bytes, slot_b, slot_b_bytes, &selected));
    TEST_ASSERT(1U == selected.slot_index);
    TEST_ASSERT(1U == selected.generation);

    return 0;
}

static int test_argument_guards (void)
{
    static uint8_t payload[STORAGE_JOURNAL_MAX_PAYLOAD_BYTES + 1U];
    static uint8_t config_payload[STORAGE_CONFIG_MAX_PAYLOAD_BYTES + 1U];
    storage_journal_queue_t queue;
    storage_journal_record_t const * p_record = NULL;
    storage_journal_view_t view;
    storage_config_view_t config_view;
    uint8_t data[STORAGE_JOURNAL_HEADER_BYTES + STORAGE_JOURNAL_MAX_PAYLOAD_BYTES] = {0};
    uint32_t output_bytes = 0U;

    storage_journal_init(&queue);
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_journal_enqueue(
        NULL, 1U, 1U, 1U, data, 1U));
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_journal_enqueue(
        &queue, 0U, 1U, 1U, data, 1U));
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_journal_enqueue(
        &queue, 1U, 1U, 1U, NULL, 1U));
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_journal_enqueue(
        &queue,
        1U,
        1U,
        1U,
        payload,
        STORAGE_JOURNAL_MAX_PAYLOAD_BYTES + 1U));
    TEST_ASSERT(APP_STATUS_OK == storage_journal_enqueue(
        &queue,
        1U,
        1U,
        1U,
        payload,
        STORAGE_JOURNAL_MAX_PAYLOAD_BYTES));
    TEST_ASSERT(APP_STATUS_OK == storage_journal_peek(&queue, &p_record));
    TEST_ASSERT(APP_STATUS_BUSY == storage_journal_encode(
        p_record,
        data,
        STORAGE_JOURNAL_HEADER_BYTES + STORAGE_JOURNAL_MAX_PAYLOAD_BYTES - 1U,
        &output_bytes));
    TEST_ASSERT(0U == output_bytes);
    TEST_ASSERT(APP_STATUS_OK == storage_journal_encode(
        p_record,
        data,
        (uint32_t) sizeof(data),
        &output_bytes));
    TEST_ASSERT(sizeof(data) == output_bytes);
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_journal_peek(NULL, &p_record));
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_journal_commit(NULL));
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_journal_decode(
        NULL, 0U, &view, &output_bytes));
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_config_validate(
        NULL, 0U, &config_view));
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_config_encode(
        1U,
        config_payload,
        STORAGE_CONFIG_MAX_PAYLOAD_BYTES + 1U,
        data,
        (uint32_t) sizeof(data),
        &output_bytes));
    TEST_ASSERT(0U == output_bytes);
    TEST_ASSERT(APP_STATUS_OK == storage_config_encode(
        1U,
        config_payload,
        STORAGE_CONFIG_MAX_PAYLOAD_BYTES,
        data,
        (uint32_t) sizeof(data),
        &output_bytes));
    TEST_ASSERT((STORAGE_CONFIG_HEADER_BYTES + STORAGE_CONFIG_MAX_PAYLOAD_BYTES) ==
                output_bytes);
    TEST_ASSERT(APP_STATUS_INVALID_ARGUMENT == storage_config_select(
        data, sizeof(data), data, sizeof(data), NULL));

    return 0;
}

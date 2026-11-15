#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "crc32_ieee.h"

#define TEST_RANDOM_BYTES    (614400U)

#define TEST_CHECK(expression)                                                         \
    do                                                                                 \
    {                                                                                  \
        if (!(expression))                                                             \
        {                                                                              \
            (void) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression);       \
            return false;                                                              \
        }                                                                              \
    } while (0)

static uint8_t g_random_data[TEST_RANDOM_BYTES]
    __attribute__((aligned(CRC32_IEEE_ALIGNMENT)));

static bool test_lifecycle_and_self_test(void);
static bool test_golden_vectors(void);
static bool test_invalid_geometry(void);
static void fill_random_data(void);

int main (void)
{
    if (!test_lifecycle_and_self_test() ||
        !test_golden_vectors() ||
        !test_invalid_geometry())
    {
        return 1;
    }

    if (APP_STATUS_OK != crc32_ieee_close())
    {
        (void) printf("FAIL %s:%d: final crc32_ieee_close\n", __FILE__, __LINE__);
        return 1;
    }
    (void) printf("crc32-ieee status=ok self_test=0x%08lX random_bytes=%u\n",
                  (unsigned long) CRC32_IEEE_SELF_TEST_EXPECTED,
                  (unsigned int) TEST_RANDOM_BYTES);
    return 0;
}

static bool test_lifecycle_and_self_test (void)
{
    static uint8_t const check[CRC32_IEEE_SELF_TEST_BYTES]
        __attribute__((aligned(CRC32_IEEE_ALIGNMENT))) =
    {
        '1', '2', '3', '4', '5', '6', '7', '8', '9', 0U, 0U, 0U,
    };
    crc32_ieee_diagnostics_t diagnostics;
    uint32_t crc32 = 0U;

    TEST_CHECK(APP_STATUS_NOT_INITIALIZED ==
               crc32_ieee_calculate(check, CRC32_IEEE_SELF_TEST_BYTES, &crc32));
    TEST_CHECK(APP_STATUS_OK == crc32_ieee_open());
    crc32_ieee_get_diagnostics(&diagnostics);
    TEST_CHECK(diagnostics.initialized);
    TEST_CHECK(1U == diagnostics.open_attempts);
    TEST_CHECK(1U == diagnostics.references);
    TEST_CHECK(1U == diagnostics.self_tests);
    TEST_CHECK(0U == diagnostics.self_test_failures);
    TEST_CHECK(1U == diagnostics.calculations);
    TEST_CHECK(CRC32_IEEE_SELF_TEST_EXPECTED == diagnostics.last_crc32);
    TEST_CHECK(APP_STATUS_OK == crc32_ieee_open());
    crc32_ieee_get_diagnostics(&diagnostics);
    TEST_CHECK(2U == diagnostics.open_attempts);
    TEST_CHECK(2U == diagnostics.references);
    TEST_CHECK(APP_STATUS_OK ==
               crc32_ieee_calculate(check, CRC32_IEEE_SELF_TEST_BYTES, &crc32));
    TEST_CHECK(CRC32_IEEE_SELF_TEST_EXPECTED == crc32);

    return true;
}

static bool test_golden_vectors (void)
{
    typedef struct st_crc32_golden_vector
    {
        uint32_t length;
        uint32_t expected;
    } crc32_golden_vector_t;

    /* Generated independently with Python zlib.crc32 over the deterministic data stream. */
    static crc32_golden_vector_t const vectors[] =
    {
        {1U,                 0xD6D9C998UL},
        {2U,                 0x126E5AE9UL},
        {3U,                 0x49744367UL},
        {4U,                 0x41426453UL},
        {8U,                 0xDCE7CB2CUL},
        {12U,                0xE1CA0B84UL},
        {16U,                0x40DD8E5DUL},
        {32U,                0x7B0DB638UL},
        {256U,               0xF67CECCCUL},
        {1024U,              0x7019DD32UL},
        {4096U,              0xEFD8E06FUL},
        {6174U,              0x37FA9F08UL},
        {65536U,             0x8778B2D3UL},
        {TEST_RANDOM_BYTES,  0x26C63787UL},
    };

    fill_random_data();
    for (uint32_t index = 0U; index < (uint32_t) (sizeof(vectors) / sizeof(vectors[0])); index++)
    {
        uint32_t hardware_crc32 = 0U;

        TEST_CHECK(APP_STATUS_OK == crc32_ieee_calculate(g_random_data,
                                                          vectors[index].length,
                                                          &hardware_crc32));
        TEST_CHECK(vectors[index].expected == hardware_crc32);
    }

    return true;
}

static bool test_invalid_geometry (void)
{
    uint32_t crc32 = 0U;

    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == crc32_ieee_calculate(NULL, 4U, &crc32));
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == crc32_ieee_calculate(g_random_data, 0U, &crc32));
    TEST_CHECK(APP_STATUS_OK == crc32_ieee_calculate(g_random_data, 3U, &crc32));
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == crc32_ieee_calculate(&g_random_data[1], 4U, &crc32));
    TEST_CHECK(APP_STATUS_INVALID_ARGUMENT == crc32_ieee_calculate(g_random_data, 4U, NULL));

    TEST_CHECK(APP_STATUS_OK == crc32_ieee_close());
    TEST_CHECK(APP_STATUS_OK == crc32_ieee_calculate(g_random_data, 4U, &crc32));
    TEST_CHECK(APP_STATUS_OK == crc32_ieee_close());
    TEST_CHECK(APP_STATUS_NOT_INITIALIZED == crc32_ieee_calculate(g_random_data, 4U, &crc32));
    TEST_CHECK(APP_STATUS_OK == crc32_ieee_open());

    return true;
}

static void fill_random_data (void)
{
    uint32_t state = 0xC001CAFEUL;

    for (uint32_t index = 0U; index < TEST_RANDOM_BYTES; index++)
    {
        state = (state * 1664525UL) + 1013904223UL;
        g_random_data[index] = (uint8_t) (state >> 24U);
    }
}

#include "crc32_ieee.h"

#include <limits.h>
#include <stddef.h>

#if defined(CRC32_IEEE_HOST_TEST)
 #define CRC32_IEEE_ALIGN_4    __attribute__((aligned(CRC32_IEEE_ALIGNMENT)))
#else
 #include "hal_data.h"
 #define CRC32_IEEE_ALIGN_4    BSP_ALIGN_VARIABLE(CRC32_IEEE_ALIGNMENT)
#endif

static uint8_t const g_crc32_ieee_self_test[CRC32_IEEE_SELF_TEST_BYTES]
    CRC32_IEEE_ALIGN_4 =
{
    '1', '2', '3', '4', '5', '6', '7', '8', '9', 0U, 0U, 0U,
};

static crc32_ieee_diagnostics_t g_diagnostics;

static app_status_t crc32_ieee_backend_open(void);
static app_status_t crc32_ieee_backend_calculate(uint8_t const * p_data,
                                                  uint32_t length,
                                                  uint32_t * p_raw_crc32);
static app_status_t crc32_ieee_backend_close(void);
static uint32_t     crc32_ieee_software_update(uint32_t raw_crc32,
                                               uint8_t const * p_data,
                                               uint32_t length);
static app_status_t crc32_ieee_record(app_status_t status);

app_status_t crc32_ieee_open (void)
{
    uint32_t     self_test_crc32 = 0U;
    app_status_t status;

    if (g_diagnostics.initialized)
    {
        if (UINT32_MAX == g_diagnostics.references)
        {
            g_diagnostics.errors++;
            return crc32_ieee_record(APP_STATUS_BUSY);
        }

        g_diagnostics.open_attempts++;
        g_diagnostics.references++;
        return crc32_ieee_record(APP_STATUS_OK);
    }

    g_diagnostics = (crc32_ieee_diagnostics_t) {0};
    g_diagnostics.open_attempts = 1U;
    status = crc32_ieee_backend_open();
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.errors++;
        return crc32_ieee_record(status);
    }

    g_diagnostics.initialized = true;
    g_diagnostics.references = 1U;
    g_diagnostics.self_tests++;
    status = crc32_ieee_calculate(g_crc32_ieee_self_test,
                                  CRC32_IEEE_SELF_TEST_BYTES,
                                  &self_test_crc32);
    if ((APP_STATUS_OK == status) && (CRC32_IEEE_SELF_TEST_EXPECTED != self_test_crc32))
    {
        status = APP_STATUS_DATA;
        g_diagnostics.errors++;
    }
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.self_test_failures++;
        (void) crc32_ieee_backend_close();
        g_diagnostics.initialized = false;
        g_diagnostics.references = 0U;
        return crc32_ieee_record(status);
    }

    return crc32_ieee_record(APP_STATUS_OK);
}

app_status_t crc32_ieee_calculate (uint8_t const * p_data, uint32_t length, uint32_t * p_crc32)
{
    uint32_t const aligned_length = length & ~(CRC32_IEEE_ALIGNMENT - 1U);
    uint32_t     raw_crc32 = 0U;
    app_status_t status;

    if ((NULL == p_data) || (NULL == p_crc32) || (0U == length) ||
        (0U != ((uintptr_t) p_data & (CRC32_IEEE_ALIGNMENT - 1U))))
    {
        return crc32_ieee_record(APP_STATUS_INVALID_ARGUMENT);
    }

    if (!g_diagnostics.initialized)
    {
        return crc32_ieee_record(APP_STATUS_NOT_INITIALIZED);
    }

    raw_crc32 = UINT32_MAX;
    if (0U != aligned_length)
    {
        status = crc32_ieee_backend_calculate(p_data, aligned_length, &raw_crc32);
        if (APP_STATUS_OK != status)
        {
            g_diagnostics.errors++;
            return crc32_ieee_record(status);
        }
    }
    raw_crc32 = crc32_ieee_software_update(raw_crc32,
                                            &p_data[aligned_length],
                                            length - aligned_length);

    *p_crc32 = ~raw_crc32;
    g_diagnostics.calculations++;
    g_diagnostics.last_crc32 = *p_crc32;

    return crc32_ieee_record(APP_STATUS_OK);
}

app_status_t crc32_ieee_close (void)
{
    app_status_t status;

    if (!g_diagnostics.initialized)
    {
        return APP_STATUS_OK;
    }

    if (g_diagnostics.references > 1U)
    {
        g_diagnostics.references--;
        return crc32_ieee_record(APP_STATUS_OK);
    }

    status = crc32_ieee_backend_close();
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.errors++;
    }
    g_diagnostics.initialized = false;
    g_diagnostics.references = 0U;

    return crc32_ieee_record(status);
}

void crc32_ieee_get_diagnostics (crc32_ieee_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        *p_diagnostics = g_diagnostics;
    }
}

#if defined(CRC32_IEEE_HOST_TEST)

static app_status_t crc32_ieee_backend_open (void)
{
    g_diagnostics.last_driver_error = 0;
    return APP_STATUS_OK;
}

static app_status_t crc32_ieee_backend_calculate (uint8_t const * p_data,
                                                   uint32_t length,
                                                   uint32_t * p_raw_crc32)
{
    uint32_t crc = UINT32_MAX;

    for (uint32_t index = 0U; index < length; index++)
    {
        crc ^= p_data[index];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            uint32_t const mask = (uint32_t) (0U - (crc & 1U));

            crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
        }
    }

    *p_raw_crc32 = crc;
    g_diagnostics.last_driver_error = 0;
    return APP_STATUS_OK;
}

static app_status_t crc32_ieee_backend_close (void)
{
    g_diagnostics.last_driver_error = 0;
    return APP_STATUS_OK;
}

#else

static app_status_t crc32_ieee_backend_open (void)
{
    fsp_err_t err;

    if ((CRC_POLYNOMIAL_CRC_32 != g_crc0_cfg.polynomial) ||
        (CRC_BIT_ORDER_LMS_LSB != g_crc0_cfg.bit_order))
    {
        g_diagnostics.last_driver_error = (int32_t) FSP_ERR_INVALID_ARGUMENT;
        return APP_STATUS_DATA;
    }

    err = g_crc0.p_api->open(g_crc0.p_ctrl, g_crc0.p_cfg);
    g_diagnostics.last_driver_error = (int32_t) err;

    return (FSP_SUCCESS == err) ? APP_STATUS_OK : APP_STATUS_IO;
}

static app_status_t crc32_ieee_backend_calculate (uint8_t const * p_data,
                                                   uint32_t length,
                                                   uint32_t * p_raw_crc32)
{
    crc_input_t input =
    {
        .num_bytes = length,
        .crc_seed = UINT32_MAX,
        .p_input_buffer = p_data,
    };
    fsp_err_t const err = g_crc0.p_api->calculate(g_crc0.p_ctrl, &input, p_raw_crc32);

    g_diagnostics.last_driver_error = (int32_t) err;
    return (FSP_SUCCESS == err) ? APP_STATUS_OK : APP_STATUS_IO;
}

static app_status_t crc32_ieee_backend_close (void)
{
    fsp_err_t const err = g_crc0.p_api->close(g_crc0.p_ctrl);

    g_diagnostics.last_driver_error = (int32_t) err;
    return (FSP_SUCCESS == err) ? APP_STATUS_OK : APP_STATUS_IO;
}

#endif

static uint32_t crc32_ieee_software_update (uint32_t raw_crc32,
                                            uint8_t const * p_data,
                                            uint32_t length)
{
    for (uint32_t index = 0U; index < length; index++)
    {
        raw_crc32 ^= p_data[index];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            uint32_t const mask = (uint32_t) (0U - (raw_crc32 & 1U));

            raw_crc32 = (raw_crc32 >> 1U) ^ (0xEDB88320UL & mask);
        }
    }

    return raw_crc32;
}

static app_status_t crc32_ieee_record (app_status_t status)
{
    g_diagnostics.last_status = status;
    if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
    {
        g_diagnostics.last_error_status = status;
    }
    return status;
}

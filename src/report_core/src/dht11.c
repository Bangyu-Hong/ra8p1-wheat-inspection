#include "dht11.h"

#include "board_port.h"

static app_status_t wait_for_level(bool level, uint32_t timeout_us)
{
    uint32_t start = board_time_us();
    while (board_dht_read() != level)
    {
        if ((uint32_t) (board_time_us() - start) >= timeout_us)
        {
            return APP_ERR_TIMEOUT;
        }
    }
    return APP_OK;
}

static app_status_t measure_level_width(bool level, uint32_t timeout_us, uint32_t * width_us)
{
    uint32_t start;
    app_status_t status;

    if (width_us == NULL)
    {
        return APP_ERR_ARGUMENT;
    }
    status = wait_for_level(level, timeout_us);
    if (status != APP_OK)
    {
        return status;
    }

    start = board_time_us();
    while (board_dht_read() == level)
    {
        if ((uint32_t) (board_time_us() - start) >= timeout_us)
        {
            return APP_ERR_TIMEOUT;
        }
    }
    *width_us = (uint32_t) (board_time_us() - start);
    return APP_OK;
}

app_status_t dht11_read(environment_t * environment)
{
    uint8_t bytes[5] = {0U};
    uint32_t pulse_width_us;
    app_status_t status = APP_OK;
    uint8_t bit;

    if (environment == NULL)
    {
        return APP_ERR_ARGUMENT;
    }
    environment->valid = false;

    board_dht_set_output();
    board_dht_write(false);
    board_delay_us(18000U);
    board_dht_write(true);
    board_delay_us(30U);
    board_dht_set_input_pullup();

    board_critical_enter();

    status = measure_level_width(false, 120U, &pulse_width_us);
    if (status == APP_OK)
    {
        status = measure_level_width(true, 120U, &pulse_width_us);
    }

    for (bit = 0U; (bit < 40U) && (status == APP_OK); ++bit)
    {
        status = measure_level_width(false, 80U, &pulse_width_us);
        if (status != APP_OK)
        {
            break;
        }
        status = measure_level_width(true, 100U, &pulse_width_us);
        if (status != APP_OK)
        {
            break;
        }

        bytes[bit / 8U] <<= 1U;
        if (pulse_width_us > 50U)
        {
            bytes[bit / 8U] |= 1U;
        }
    }

    board_critical_exit();

    if (status != APP_OK)
    {
        return status;
    }
    if ((uint8_t) (bytes[0] + bytes[1] + bytes[2] + bytes[3]) != bytes[4])
    {
        return APP_ERR_CHECKSUM;
    }
    if ((bytes[0] > 100U) || ((bytes[2] & 0x7FU) > 80U))
    {
        return APP_ERR_PROTOCOL;
    }

    environment->humidity_percent = bytes[0];
    environment->temperature_c = (int8_t) (bytes[2] & 0x7FU);
    if ((bytes[2] & 0x80U) != 0U)
    {
        environment->temperature_c = (int8_t) -environment->temperature_c;
    }
    environment->valid = true;
    return APP_OK;
}

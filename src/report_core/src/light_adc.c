#include "light_adc.h"

#include "app_config.h"
#include "board_port.h"

static void insertion_sort(uint16_t * values, size_t count)
{
    size_t i;
    for (i = 1U; i < count; ++i)
    {
        uint16_t key = values[i];
        size_t j = i;
        while ((j > 0U) && (values[j - 1U] > key))
        {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = key;
    }
}

app_status_t light_adc_read_percent(uint8_t * light_percent)
{
    static uint32_t filtered_q8;
    static bool filter_initialized;
    uint16_t samples[LIGHT_FILTER_SAMPLES];
    uint16_t median;
    int32_t percent;
    size_t index;
    app_status_t status;

    if ((light_percent == NULL) || (LIGHT_ADC_DARK_CAL <= LIGHT_ADC_BRIGHT_CAL))
    {
        return APP_ERR_ARGUMENT;
    }

    for (index = 0U; index < LIGHT_FILTER_SAMPLES; ++index)
    {
        status = board_light_adc_read(&samples[index]);
        if (status != APP_OK)
        {
            return status;
        }
        board_delay_us(1000U);
    }

    insertion_sort(samples, LIGHT_FILTER_SAMPLES);
    median = samples[LIGHT_FILTER_SAMPLES / 2U];

    if (!filter_initialized)
    {
        filtered_q8 = (uint32_t) median << 8U;
        filter_initialized = true;
    }
    else
    {
        /* Exponential moving average: new = 1/4 sample + 3/4 old. */
        filtered_q8 = (filtered_q8 * 3U + ((uint32_t) median << 8U)) / 4U;
    }

    percent = ((int32_t) LIGHT_ADC_DARK_CAL - (int32_t) (filtered_q8 >> 8U)) * 100;
    percent /= (int32_t) (LIGHT_ADC_DARK_CAL - LIGHT_ADC_BRIGHT_CAL);
    if (percent < 0)
    {
        percent = 0;
    }
    else if (percent > 100)
    {
        percent = 100;
    }

    *light_percent = (uint8_t) percent;
    return APP_OK;
}

bool light_adc_is_acceptable(uint8_t light_percent)
{
    return (light_percent >= LIGHT_ACCEPT_MIN_PERCENT) &&
           (light_percent <= LIGHT_ACCEPT_MAX_PERCENT);
}

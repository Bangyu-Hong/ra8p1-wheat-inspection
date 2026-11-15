#ifndef LIGHT_ADC_H
#define LIGHT_ADC_H

#include "common_types.h"

app_status_t light_adc_read_percent(uint8_t * light_percent);
bool light_adc_is_acceptable(uint8_t light_percent);

#endif

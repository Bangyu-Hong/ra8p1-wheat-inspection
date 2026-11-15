#ifndef ESP_AT_RESET_H_
#define ESP_AT_RESET_H_

#include <stdbool.h>

#include "app_status.h"

app_status_t esp_at_reset_set_asserted(bool asserted);

#endif /* ESP_AT_RESET_H_ */

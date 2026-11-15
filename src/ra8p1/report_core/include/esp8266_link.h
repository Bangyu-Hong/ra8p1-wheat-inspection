#ifndef ESP8266_LINK_H
#define ESP8266_LINK_H

#include "common_types.h"

app_status_t esp8266_link_init(void);
bool esp8266_link_is_online(void);
app_status_t esp8266_send_record(const detection_record_t * record);

#endif

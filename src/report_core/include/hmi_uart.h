#ifndef HMI_UART_H
#define HMI_UART_H

#include "common_types.h"

typedef enum
{
    HMI_EVENT_NONE = 0,
    HMI_EVENT_START,
    HMI_EVENT_CANCEL
} hmi_event_t;

app_status_t hmi_init(void);
app_status_t hmi_set_status(const char * text);
app_status_t hmi_show_result(const detection_record_t * record);
hmi_event_t hmi_poll_event(char * sample_id, size_t sample_id_capacity);

#endif

#include "fault_record.h"

#include <stddef.h>

void fault_record_queue_init (fault_record_queue_t * p_queue)
{
    if (NULL != p_queue)
    {
        *p_queue = (fault_record_queue_t) {0};
    }
}

app_status_t fault_record_push (fault_record_queue_t * p_queue,
                                fault_record_t const * p_record)
{
    if ((NULL == p_queue) || (NULL == p_record))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    if (FAULT_RECORD_CAPACITY == p_queue->used)
    {
        p_queue->tail = (uint8_t) ((p_queue->tail + 1U) % FAULT_RECORD_CAPACITY);
        p_queue->used--;
        p_queue->dropped_oldest++;
    }
    p_queue->records[p_queue->head] = *p_record;
    p_queue->head = (uint8_t) ((p_queue->head + 1U) % FAULT_RECORD_CAPACITY);
    p_queue->used++;
    p_queue->pushed++;
    if (p_queue->used > p_queue->high_water)
    {
        p_queue->high_water = p_queue->used;
    }

    return APP_STATUS_OK;
}

app_status_t fault_record_pop (fault_record_queue_t * p_queue,
                               fault_record_t * p_record,
                               bool * p_available)
{
    if ((NULL == p_queue) || (NULL == p_record) || (NULL == p_available))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    *p_available = false;
    *p_record = (fault_record_t) {0};
    if (0U < p_queue->used)
    {
        *p_record = p_queue->records[p_queue->tail];
        p_queue->tail = (uint8_t) ((p_queue->tail + 1U) % FAULT_RECORD_CAPACITY);
        p_queue->used--;
        p_queue->popped++;
        *p_available = true;
    }

    return APP_STATUS_OK;
}

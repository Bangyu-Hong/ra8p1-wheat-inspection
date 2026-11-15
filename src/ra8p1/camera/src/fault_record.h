#ifndef FAULT_RECORD_H_
#define FAULT_RECORD_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define FAULT_RECORD_CAPACITY  (16U)

typedef struct st_fault_record
{
    uint32_t sequence;
    uint64_t monotonic_us;
    uint16_t subsystem;
    uint16_t operation;
    app_status_t status;
    int32_t detail;
} fault_record_t;

typedef struct st_fault_record_queue
{
    fault_record_t records[FAULT_RECORD_CAPACITY];
    uint8_t head;
    uint8_t tail;
    uint8_t used;
    uint32_t pushed;
    uint32_t popped;
    uint32_t dropped_oldest;
    uint32_t high_water;
} fault_record_queue_t;

void fault_record_queue_init(fault_record_queue_t * p_queue);
app_status_t fault_record_push(fault_record_queue_t * p_queue,
                               fault_record_t const * p_record);
app_status_t fault_record_pop(fault_record_queue_t * p_queue,
                              fault_record_t * p_record,
                              bool * p_available);

#endif /* FAULT_RECORD_H_ */

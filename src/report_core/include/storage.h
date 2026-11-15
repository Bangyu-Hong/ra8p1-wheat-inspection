#ifndef STORAGE_H
#define STORAGE_H

#include "common_types.h"

app_status_t storage_init(void);
app_status_t storage_save_record(const detection_record_t * record);
app_status_t storage_save_rgb565(const char * sample_id, const image_t * frame);
app_status_t storage_cache_pending_upload(const detection_record_t * record);

#endif

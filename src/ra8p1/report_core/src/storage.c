#include "storage.h"

#include "board_port.h"

#include <stdio.h>
#include <string.h>

static app_status_t append_csv_row(const char * path, const detection_record_t * record)
{
    char row[256];
    int length = snprintf(row,
                          sizeof(row),
                          "%s,%lu,%u,%d,%u,%u,%u,%.1f,%.3f,%.3f,%.1f\r\n",
                          record->sample_id,
                          (unsigned long) record->timestamp_s,
                          record->wheat_ear_count,
                          record->environment.temperature_c,
                          record->environment.humidity_percent,
                          record->environment.valid ? 1U : 0U,
                          record->light_percent,
                          (double) record->image_quality.mean_luma,
                          (double) record->image_quality.dark_ratio,
                          (double) record->image_quality.bright_ratio,
                          (double) record->image_quality.sharpness);
    if ((length < 0) || ((size_t) length >= sizeof(row)))
    {
        return APP_ERR_NO_SPACE;
    }
    return board_file_append(path, row, (size_t) length);
}

static app_status_t ensure_csv_header(const char * path)
{
    static const char header[] =
        "sample_id,timestamp,count,temp_c,humidity,env_valid,light_percent,"
        "mean_luma,dark_ratio,bright_ratio,sharpness\r\n";
    size_t file_size = 0U;
    app_status_t status = board_file_size(path, &file_size);
    if ((status != APP_OK) && (status != APP_ERR_NOT_READY))
    {
        return status;
    }
    if ((status == APP_ERR_NOT_READY) || (file_size == 0U))
    {
        return board_file_append(path, header, sizeof(header) - 1U);
    }
    return APP_OK;
}

app_status_t storage_init(void)
{
    if (!board_storage_ready())
    {
        return APP_ERR_NOT_READY;
    }
    return ensure_csv_header("/data/results.csv");
}

app_status_t storage_save_record(const detection_record_t * record)
{
    if ((record == NULL) || !board_storage_ready())
    {
        return APP_ERR_NOT_READY;
    }
    return append_csv_row("/data/results.csv", record);
}

app_status_t storage_save_rgb565(const char * sample_id, const image_t * frame)
{
    char path[64];
    int length;
    if ((sample_id == NULL) || (frame == NULL) || (frame->data == NULL) || !board_storage_ready())
    {
        return APP_ERR_NOT_READY;
    }
    length = snprintf(path, sizeof(path), "/images/%s_%lu.rgb565", sample_id,
                      (unsigned long) board_rtc_timestamp_s());
    if ((length < 0) || ((size_t) length >= sizeof(path)))
    {
        return APP_ERR_NO_SPACE;
    }
    return board_file_write(path, frame->data, (size_t) frame->stride_bytes * frame->height);
}

app_status_t storage_cache_pending_upload(const detection_record_t * record)
{
    app_status_t status;
    if ((record == NULL) || !board_storage_ready())
    {
        return APP_ERR_NOT_READY;
    }
    status = ensure_csv_header("/cache/pending.csv");
    return (status == APP_OK) ? append_csv_row("/cache/pending.csv", record) : status;
}

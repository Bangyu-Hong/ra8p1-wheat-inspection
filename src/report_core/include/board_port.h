#ifndef BOARD_PORT_H
#define BOARD_PORT_H

#include "common_types.h"

void board_delay_us(uint32_t delay_us);
uint32_t board_time_us(void);
uint32_t board_time_ms(void);
uint32_t board_rtc_timestamp_s(void);
void board_critical_enter(void);
void board_critical_exit(void);

app_status_t board_sccb_write(uint8_t device_addr, uint16_t reg, uint8_t value);
app_status_t board_sccb_read(uint8_t device_addr, uint16_t reg, uint8_t * value);
app_status_t board_camera_start(uint8_t * destination, size_t capacity);
bool board_camera_frame_done(void);
void board_camera_abort(void);

void board_dht_set_output(void);
void board_dht_set_input_pullup(void);
void board_dht_write(bool high);
bool board_dht_read(void);

app_status_t board_light_adc_read(uint16_t * raw);

app_status_t board_hmi_uart_write(const uint8_t * data, size_t length);
size_t board_hmi_uart_read(uint8_t * data, size_t capacity);
app_status_t board_esp_uart_write(const uint8_t * data, size_t length);
size_t board_esp_uart_read(uint8_t * data, size_t capacity);

bool board_storage_ready(void);
app_status_t board_file_size(const char * path, size_t * size);
app_status_t board_file_append(const char * path, const void * data, size_t length);
app_status_t board_file_write(const char * path, const void * data, size_t length);

#endif

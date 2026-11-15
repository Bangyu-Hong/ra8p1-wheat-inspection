#ifndef ESP_AT_PARSER_H_
#define ESP_AT_PARSER_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define ESP_AT_PARSER_LINE_BYTES      (384U)
#define ESP_AT_PARSER_MAX_IPD_BYTES   (8192U)
#define ESP_AT_PARSER_MAX_LINK_ID     (4U)

typedef app_status_t (*esp_at_line_handler_t)(char const * p_line,
                                               uint32_t line_bytes,
                                               void * p_context);
typedef app_status_t (*esp_at_ipd_handler_t)(uint8_t link_id,
                                              uint8_t const * p_data,
                                              uint32_t data_bytes,
                                              void * p_context);

typedef struct st_esp_at_parser_diagnostics
{
    uint32_t bytes_received;
    uint32_t lines_received;
    uint32_t prompts_received;
    uint32_t ipd_frames;
    uint32_t ipd_bytes;
    uint32_t malformed_ipd;
    uint32_t oversized_ipd;
    uint32_t line_overflows;
    uint32_t discarded_bytes;
    uint32_t callback_errors;
    uint32_t buffered_line_bytes;
    uint32_t ipd_bytes_remaining;
    app_status_t last_status;
} esp_at_parser_diagnostics_t;

typedef struct st_esp_at_parser
{
    char line[ESP_AT_PARSER_LINE_BYTES];
    uint32_t line_used;
    uint32_t ipd_remaining;
    uint32_t ipd_discard_remaining;
    uint8_t ipd_link_id;
    bool discard_until_newline;
    bool skip_prompt_space;
    esp_at_parser_diagnostics_t diagnostics;
} esp_at_parser_t;

void esp_at_parser_init(esp_at_parser_t * p_parser);
app_status_t esp_at_parser_feed(esp_at_parser_t * p_parser,
                                uint8_t const * p_data,
                                uint32_t data_bytes,
                                esp_at_line_handler_t line_handler,
                                esp_at_ipd_handler_t ipd_handler,
                                void * p_context);
void esp_at_parser_get_diagnostics(esp_at_parser_t const * p_parser,
                                   esp_at_parser_diagnostics_t * p_diagnostics);

#endif /* ESP_AT_PARSER_H_ */

#include "esp_at_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static app_status_t esp_at_parser_emit_line(esp_at_parser_t * p_parser,
                                            esp_at_line_handler_t handler,
                                            void * p_context);
static bool esp_at_parser_parse_ipd(char const * p_header,
                                    uint32_t header_bytes,
                                    uint8_t * p_link_id,
                                    uint32_t * p_payload_bytes);
static bool esp_at_parser_parse_u32(char const * p_text,
                                    uint32_t text_bytes,
                                    uint32_t * p_value);
static app_status_t esp_at_parser_record(esp_at_parser_t * p_parser,
                                         app_status_t status);

void esp_at_parser_init (esp_at_parser_t * p_parser)
{
    if (NULL != p_parser)
    {
        *p_parser = (esp_at_parser_t) {0};
        p_parser->diagnostics.last_status = APP_STATUS_OK;
    }
}

app_status_t esp_at_parser_feed (esp_at_parser_t * p_parser,
                                 uint8_t const * p_data,
                                 uint32_t data_bytes,
                                 esp_at_line_handler_t line_handler,
                                 esp_at_ipd_handler_t ipd_handler,
                                 void * p_context)
{
    uint32_t offset = 0U;
    app_status_t result = APP_STATUS_OK;

    if ((NULL == p_parser) || ((0U < data_bytes) && (NULL == p_data)))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }

    while (offset < data_bytes)
    {
        if (0U < p_parser->ipd_discard_remaining)
        {
            uint32_t chunk_bytes = data_bytes - offset;

            if (chunk_bytes > p_parser->ipd_discard_remaining)
            {
                chunk_bytes = p_parser->ipd_discard_remaining;
            }
            offset += chunk_bytes;
            p_parser->ipd_discard_remaining -= chunk_bytes;
            p_parser->diagnostics.bytes_received += chunk_bytes;
            p_parser->diagnostics.discarded_bytes += chunk_bytes;
            p_parser->diagnostics.ipd_bytes_remaining = p_parser->ipd_discard_remaining;
            continue;
        }
        if (0U < p_parser->ipd_remaining)
        {
            uint32_t chunk_bytes = data_bytes - offset;
            app_status_t status;

            if (chunk_bytes > p_parser->ipd_remaining)
            {
                chunk_bytes = p_parser->ipd_remaining;
            }
            status = (NULL == ipd_handler) ? APP_STATUS_OK :
                     ipd_handler(p_parser->ipd_link_id,
                                 &p_data[offset],
                                 chunk_bytes,
                                 p_context);
            offset += chunk_bytes;
            p_parser->diagnostics.bytes_received += chunk_bytes;
            p_parser->ipd_remaining -= chunk_bytes;
            p_parser->diagnostics.ipd_bytes += chunk_bytes;
            p_parser->diagnostics.ipd_bytes_remaining = p_parser->ipd_remaining;
            if (APP_STATUS_OK != status)
            {
                p_parser->diagnostics.callback_errors++;
                return esp_at_parser_record(p_parser, status);
            }
            continue;
        }

        {
            uint8_t const byte = p_data[offset++];

            p_parser->diagnostics.bytes_received++;
            if (p_parser->discard_until_newline)
            {
                p_parser->diagnostics.discarded_bytes++;
                if ('\n' == (char) byte)
                {
                    p_parser->discard_until_newline = false;
                }
                continue;
            }
            if (p_parser->skip_prompt_space)
            {
                p_parser->skip_prompt_space = false;
                if (' ' == (char) byte)
                {
                    continue;
                }
            }
            if ('\r' == (char) byte)
            {
                continue;
            }
            if (('>' == (char) byte) && (0U == p_parser->line_used))
            {
                app_status_t const status = (NULL == line_handler) ? APP_STATUS_OK :
                                            line_handler(">", 1U, p_context);

                p_parser->diagnostics.prompts_received++;
                p_parser->skip_prompt_space = true;
                if (APP_STATUS_OK != status)
                {
                    p_parser->diagnostics.callback_errors++;
                    return esp_at_parser_record(p_parser, status);
                }
                continue;
            }
            if ('\n' == (char) byte)
            {
                app_status_t const status = esp_at_parser_emit_line(p_parser,
                                                                     line_handler,
                                                                     p_context);

                if (APP_STATUS_OK != status)
                {
                    return status;
                }
                continue;
            }
            if (p_parser->line_used >= (ESP_AT_PARSER_LINE_BYTES - 1U))
            {
                p_parser->diagnostics.discarded_bytes += p_parser->line_used + 1U;
                p_parser->line_used = 0U;
                p_parser->line[0] = '\0';
                p_parser->discard_until_newline = true;
                p_parser->diagnostics.line_overflows++;
                p_parser->diagnostics.buffered_line_bytes = 0U;
                result = APP_STATUS_PROTOCOL;
                continue;
            }

            p_parser->line[p_parser->line_used++] = (char) byte;
            p_parser->line[p_parser->line_used] = '\0';
            p_parser->diagnostics.buffered_line_bytes = p_parser->line_used;
            if ((':' == (char) byte) && (p_parser->line_used >= 7U) &&
                (0 == memcmp(p_parser->line, "+IPD,", 5U)))
            {
                uint8_t link_id = 0U;
                uint32_t payload_bytes = 0U;

                if (!esp_at_parser_parse_ipd(p_parser->line,
                                             p_parser->line_used,
                                             &link_id,
                                             &payload_bytes))
                {
                    p_parser->diagnostics.discarded_bytes += p_parser->line_used;
                    p_parser->line_used = 0U;
                    p_parser->line[0] = '\0';
                    p_parser->discard_until_newline = true;
                    p_parser->diagnostics.malformed_ipd++;
                    p_parser->diagnostics.buffered_line_bytes = 0U;
                    result = APP_STATUS_PROTOCOL;
                    continue;
                }
                if (payload_bytes > ESP_AT_PARSER_MAX_IPD_BYTES)
                {
                    p_parser->line_used = 0U;
                    p_parser->line[0] = '\0';
                    p_parser->ipd_discard_remaining = payload_bytes;
                    p_parser->diagnostics.oversized_ipd++;
                    p_parser->diagnostics.buffered_line_bytes = 0U;
                    p_parser->diagnostics.ipd_bytes_remaining = payload_bytes;
                    result = APP_STATUS_PROTOCOL;
                    continue;
                }

                p_parser->line_used = 0U;
                p_parser->line[0] = '\0';
                p_parser->ipd_link_id = link_id;
                p_parser->ipd_remaining = payload_bytes;
                p_parser->diagnostics.ipd_frames++;
                p_parser->diagnostics.buffered_line_bytes = 0U;
                p_parser->diagnostics.ipd_bytes_remaining = payload_bytes;
            }
        }
    }

    return esp_at_parser_record(p_parser, result);
}

void esp_at_parser_get_diagnostics (esp_at_parser_t const * p_parser,
                                    esp_at_parser_diagnostics_t * p_diagnostics)
{
    if ((NULL != p_parser) && (NULL != p_diagnostics))
    {
        *p_diagnostics = p_parser->diagnostics;
    }
}

static app_status_t esp_at_parser_emit_line (esp_at_parser_t * p_parser,
                                             esp_at_line_handler_t handler,
                                             void * p_context)
{
    app_status_t status = APP_STATUS_OK;

    if (0U < p_parser->line_used)
    {
        if (NULL != handler)
        {
            status = handler(p_parser->line, p_parser->line_used, p_context);
        }
        p_parser->diagnostics.lines_received++;
        if (APP_STATUS_OK != status)
        {
            p_parser->diagnostics.callback_errors++;
        }
    }
    p_parser->line_used = 0U;
    p_parser->line[0] = '\0';
    p_parser->diagnostics.buffered_line_bytes = 0U;

    return esp_at_parser_record(p_parser, status);
}

static bool esp_at_parser_parse_ipd (char const * p_header,
                                     uint32_t header_bytes,
                                     uint8_t * p_link_id,
                                     uint32_t * p_payload_bytes)
{
    uint32_t token_start[4U];
    uint32_t token_bytes[4U] = {0U};
    uint32_t token_count = 1U;
    uint32_t payload_index;
    uint32_t link_id = 0U;
    uint32_t payload_bytes;
    uint32_t index;

    if ((NULL == p_header) || (NULL == p_link_id) || (NULL == p_payload_bytes) ||
        (header_bytes < 7U) || (':' != p_header[header_bytes - 1U]) ||
        (0 != memcmp(p_header, "+IPD,", 5U)))
    {
        return false;
    }

    token_start[0] = 5U;
    for (index = 5U; index < (header_bytes - 1U); index++)
    {
        if (',' == p_header[index])
        {
            if ((token_count >= 4U) || (index == token_start[token_count - 1U]))
            {
                return false;
            }
            token_bytes[token_count - 1U] = index - token_start[token_count - 1U];
            token_start[token_count] = index + 1U;
            token_count++;
        }
    }
    if (token_start[token_count - 1U] == (header_bytes - 1U))
    {
        return false;
    }
    token_bytes[token_count - 1U] = (header_bytes - 1U) - token_start[token_count - 1U];

    if ((1U == token_count) || (3U == token_count))
    {
        payload_index = 0U;
    }
    else if ((2U == token_count) || (4U == token_count))
    {
        payload_index = 1U;
        if (!esp_at_parser_parse_u32(&p_header[token_start[0U]],
                                     token_bytes[0U],
                                     &link_id) ||
            (link_id > ESP_AT_PARSER_MAX_LINK_ID))
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    if (!esp_at_parser_parse_u32(&p_header[token_start[payload_index]],
                                 token_bytes[payload_index],
                                 &payload_bytes))
    {
        return false;
    }
    if (token_count >= 3U)
    {
        uint32_t const remote_index = payload_index + 1U;
        uint32_t remote_port;

        if ((0U == token_bytes[remote_index]) ||
            !esp_at_parser_parse_u32(&p_header[token_start[remote_index + 1U]],
                                     token_bytes[remote_index + 1U],
                                     &remote_port) ||
            (0U == remote_port) || (remote_port > UINT16_MAX))
        {
            return false;
        }
    }

    *p_link_id = (uint8_t) link_id;
    *p_payload_bytes = payload_bytes;
    return true;
}

static bool esp_at_parser_parse_u32 (char const * p_text,
                                     uint32_t text_bytes,
                                     uint32_t * p_value)
{
    uint32_t value = 0U;
    uint32_t index;

    if ((NULL == p_text) || (NULL == p_value) || (0U == text_bytes))
    {
        return false;
    }
    for (index = 0U; index < text_bytes; index++)
    {
        uint32_t digit;

        if ((p_text[index] < '0') || (p_text[index] > '9'))
        {
            return false;
        }
        digit = (uint32_t) (p_text[index] - '0');
        if (value > ((UINT32_MAX - digit) / 10U))
        {
            return false;
        }
        value = (value * 10U) + digit;
    }
    *p_value = value;
    return true;
}

static app_status_t esp_at_parser_record (esp_at_parser_t * p_parser,
                                          app_status_t status)
{
    p_parser->diagnostics.last_status = status;
    return status;
}

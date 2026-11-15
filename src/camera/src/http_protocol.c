#include "http_protocol.h"

#include <stddef.h>
#include <string.h>

static app_status_t http_protocol_parse(http_protocol_parser_t * p_parser,
                                        http_protocol_request_t * p_request);
static bool http_protocol_parse_request_line(char * p_line,
                                             http_protocol_request_t * p_request,
                                             bool * p_http_11);
static bool http_protocol_header_name_equals(char const * p_name,
                                             size_t name_bytes,
                                             char const * p_expected);
static bool http_protocol_value_equals(char const * p_value,
                                       size_t value_bytes,
                                       char const * p_expected);
static bool http_protocol_value_has_token(char const * p_value,
                                          size_t value_bytes,
                                          char const * p_token);
static bool http_protocol_websocket_key_valid(char const * p_value,
                                              size_t value_bytes);
static char http_protocol_ascii_lower(char value);

void http_protocol_parser_init (http_protocol_parser_t * p_parser)
{
    if (NULL != p_parser)
    {
        *p_parser = (http_protocol_parser_t) {0};
    }
}

app_status_t http_protocol_parser_feed (http_protocol_parser_t * p_parser,
                                        uint8_t const * p_data,
                                        uint32_t data_bytes,
                                        uint32_t * p_consumed_bytes,
                                        http_protocol_request_t * p_request)
{
    uint32_t index;

    if ((NULL == p_parser) || (NULL == p_consumed_bytes) || (NULL == p_request) ||
        ((0U < data_bytes) && (NULL == p_data)))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    *p_consumed_bytes = 0U;
    *p_request = (http_protocol_request_t) {0};
    if (p_parser->complete)
    {
        return APP_STATUS_UNAVAILABLE;
    }

    for (index = 0U; index < data_bytes; index++)
    {
        if (p_parser->used >= HTTP_PROTOCOL_MAX_HEADER_BYTES)
        {
            return APP_STATUS_DATA;
        }
        p_parser->header[p_parser->used] = (char) p_data[index];
        p_parser->used++;
        *p_consumed_bytes = index + 1U;
        if ((p_parser->used >= 4U) &&
            (0 == memcmp(&p_parser->header[p_parser->used - 4U], "\r\n\r\n", 4U)))
        {
            p_parser->header[p_parser->used] = '\0';
            p_parser->complete = true;
            return http_protocol_parse(p_parser, p_request);
        }
    }

    return APP_STATUS_BUSY;
}

static app_status_t http_protocol_parse (http_protocol_parser_t * p_parser,
                                         http_protocol_request_t * p_request)
{
    char * p_cursor = p_parser->header;
    char * p_line_end = strstr(p_cursor, "\r\n");
    bool http_11 = false;
    bool connection_upgrade = false;
    bool upgrade_websocket = false;
    bool version_13 = false;
    bool key_seen = false;

    if ((NULL == p_line_end) || (p_line_end == p_cursor))
    {
        return APP_STATUS_PROTOCOL;
    }
    *p_line_end = '\0';
    if (!http_protocol_parse_request_line(p_cursor, p_request, &http_11))
    {
        return APP_STATUS_PROTOCOL;
    }
    p_cursor = p_line_end + 2;

    while ('\0' != p_cursor[0])
    {
        char * const p_next = strstr(p_cursor, "\r\n");
        char * p_colon;
        char * p_value;
        size_t name_bytes;
        size_t value_bytes;

        if (NULL == p_next)
        {
            return APP_STATUS_PROTOCOL;
        }
        if (p_next == p_cursor)
        {
            break;
        }
        *p_next = '\0';
        p_colon = strchr(p_cursor, ':');
        if ((NULL == p_colon) || (p_colon == p_cursor))
        {
            return APP_STATUS_PROTOCOL;
        }
        name_bytes = (size_t) (p_colon - p_cursor);
        p_value = p_colon + 1;
        while ((' ' == *p_value) || ('\t' == *p_value))
        {
            p_value++;
        }
        value_bytes = strlen(p_value);
        while ((0U < value_bytes) &&
               ((' ' == p_value[value_bytes - 1U]) || ('\t' == p_value[value_bytes - 1U])))
        {
            value_bytes--;
        }

        if (http_protocol_header_name_equals(p_cursor, name_bytes, "connection"))
        {
            connection_upgrade = http_protocol_value_has_token(p_value,
                                                                value_bytes,
                                                                "upgrade");
        }
        else if (http_protocol_header_name_equals(p_cursor, name_bytes, "upgrade"))
        {
            upgrade_websocket = http_protocol_value_equals(p_value,
                                                            value_bytes,
                                                            "websocket");
        }
        else if (http_protocol_header_name_equals(p_cursor, name_bytes,
                                                   "sec-websocket-version"))
        {
            version_13 = http_protocol_value_equals(p_value, value_bytes, "13");
        }
        else if (http_protocol_header_name_equals(p_cursor, name_bytes,
                                                   "sec-websocket-key"))
        {
            if (key_seen || !http_protocol_websocket_key_valid(p_value, value_bytes))
            {
                return APP_STATUS_PROTOCOL;
            }
            (void) memcpy(p_request->websocket_key, p_value, value_bytes);
            p_request->websocket_key[value_bytes] = '\0';
            key_seen = true;
        }
        else if (http_protocol_header_name_equals(p_cursor, name_bytes, "content-length"))
        {
            if (!http_protocol_value_equals(p_value, value_bytes, "0"))
            {
                return APP_STATUS_PROTOCOL;
            }
        }
        else if (http_protocol_header_name_equals(p_cursor, name_bytes, "transfer-encoding"))
        {
            return APP_STATUS_PROTOCOL;
        }
        p_cursor = p_next + 2;
    }

    if (HTTP_PROTOCOL_ROUTE_WEBSOCKET == p_request->route)
    {
        if (!http_11 || !connection_upgrade || !upgrade_websocket ||
            !version_13 || !key_seen)
        {
            return APP_STATUS_PROTOCOL;
        }
        p_request->websocket_upgrade = true;
    }
    return APP_STATUS_OK;
}

static bool http_protocol_parse_request_line (char * p_line,
                                              http_protocol_request_t * p_request,
                                              bool * p_http_11)
{
    char * const p_target = strchr(p_line, ' ');
    char * p_version;

    if ((NULL == p_target) || (0 != memcmp(p_line, "GET", 3U)) ||
        ((size_t) (p_target - p_line) != 3U))
    {
        return false;
    }
    p_version = strchr(p_target + 1, ' ');
    if ((NULL == p_version) || ('\0' == p_target[1]) || ('\0' == p_version[1]))
    {
        return false;
    }
    *p_target = '\0';
    *p_version = '\0';
    p_version++;
    if (0 == strcmp(p_version, "HTTP/1.1"))
    {
        *p_http_11 = true;
    }
    else if (0 != strcmp(p_version, "HTTP/1.0"))
    {
        return false;
    }

    if (0 == strcmp(p_target + 1, "/"))
    {
        p_request->route = HTTP_PROTOCOL_ROUTE_ROOT;
    }
    else if (0 == strcmp(p_target + 1, "/health"))
    {
        p_request->route = HTTP_PROTOCOL_ROUTE_HEALTH;
    }
    else if (0 == strcmp(p_target + 1, "/ws"))
    {
        p_request->route = HTTP_PROTOCOL_ROUTE_WEBSOCKET;
    }
    else
    {
        return false;
    }
    return true;
}

static bool http_protocol_header_name_equals (char const * p_name,
                                              size_t name_bytes,
                                              char const * p_expected)
{
    size_t const expected_bytes = strlen(p_expected);
    size_t index;

    if (name_bytes != expected_bytes)
    {
        return false;
    }
    for (index = 0U; index < name_bytes; index++)
    {
        if (http_protocol_ascii_lower(p_name[index]) != p_expected[index])
        {
            return false;
        }
    }
    return true;
}

static bool http_protocol_value_equals (char const * p_value,
                                        size_t value_bytes,
                                        char const * p_expected)
{
    size_t const expected_bytes = strlen(p_expected);
    size_t index;

    if (value_bytes != expected_bytes)
    {
        return false;
    }
    for (index = 0U; index < value_bytes; index++)
    {
        if (http_protocol_ascii_lower(p_value[index]) != p_expected[index])
        {
            return false;
        }
    }
    return true;
}

static bool http_protocol_value_has_token (char const * p_value,
                                           size_t value_bytes,
                                           char const * p_token)
{
    size_t token_start = 0U;

    while (token_start < value_bytes)
    {
        size_t token_end;

        while ((token_start < value_bytes) &&
               ((',' == p_value[token_start]) || (' ' == p_value[token_start]) ||
                ('\t' == p_value[token_start])))
        {
            token_start++;
        }
        token_end = token_start;
        while ((token_end < value_bytes) && (',' != p_value[token_end]))
        {
            token_end++;
        }
        while ((token_end > token_start) &&
               ((' ' == p_value[token_end - 1U]) || ('\t' == p_value[token_end - 1U])))
        {
            token_end--;
        }
        if (http_protocol_value_equals(&p_value[token_start],
                                       token_end - token_start,
                                       p_token))
        {
            return true;
        }
        token_start = token_end + 1U;
    }
    return false;
}

static bool http_protocol_websocket_key_valid (char const * p_value,
                                               size_t value_bytes)
{
    size_t index;

    if ((24U != value_bytes) || ('=' != p_value[22]) || ('=' != p_value[23]))
    {
        return false;
    }
    for (index = 0U; index < 22U; index++)
    {
        char const value = p_value[index];
        bool const base64 = ((value >= 'A') && (value <= 'Z')) ||
                            ((value >= 'a') && (value <= 'z')) ||
                            ((value >= '0') && (value <= '9')) ||
                            ('+' == value) || ('/' == value);

        if (!base64)
        {
            return false;
        }
    }
    return true;
}

static char http_protocol_ascii_lower (char value)
{
    return ((value >= 'A') && (value <= 'Z')) ?
           (char) (value + ('a' - 'A')) : value;
}

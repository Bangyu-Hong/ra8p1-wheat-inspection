#ifndef HTTP_PROTOCOL_H_
#define HTTP_PROTOCOL_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define HTTP_PROTOCOL_MAX_HEADER_BYTES  (1024U)
#define HTTP_PROTOCOL_WEBSOCKET_KEY_BYTES  (25U)

typedef enum e_http_protocol_route
{
    HTTP_PROTOCOL_ROUTE_NONE = 0,
    HTTP_PROTOCOL_ROUTE_ROOT,
    HTTP_PROTOCOL_ROUTE_HEALTH,
    HTTP_PROTOCOL_ROUTE_WEBSOCKET
} http_protocol_route_t;

typedef struct st_http_protocol_request
{
    http_protocol_route_t route;
    char websocket_key[HTTP_PROTOCOL_WEBSOCKET_KEY_BYTES];
    bool websocket_upgrade;
} http_protocol_request_t;

typedef struct st_http_protocol_parser
{
    char header[HTTP_PROTOCOL_MAX_HEADER_BYTES + 1U];
    uint32_t used;
    bool complete;
} http_protocol_parser_t;

void http_protocol_parser_init(http_protocol_parser_t * p_parser);
app_status_t http_protocol_parser_feed(http_protocol_parser_t * p_parser,
                                       uint8_t const * p_data,
                                       uint32_t data_bytes,
                                       uint32_t * p_consumed_bytes,
                                       http_protocol_request_t * p_request);

#endif /* HTTP_PROTOCOL_H_ */

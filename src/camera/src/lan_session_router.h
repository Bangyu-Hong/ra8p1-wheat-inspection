#ifndef LAN_SESSION_ROUTER_H_
#define LAN_SESSION_ROUTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "app_telemetry.h"
#include "local_tcp_session.h"

#define LAN_SESSION_ROUTER_MAX_LINKS  (5U)
#define LAN_SESSION_ROUTER_NO_LINK    (0xFFU)

typedef enum e_lan_session_protocol
{
    LAN_SESSION_PROTOCOL_FREE = 0,
    LAN_SESSION_PROTOCOL_DETECT,
    LAN_SESSION_PROTOCOL_LEGACY,
    LAN_SESSION_PROTOCOL_HTTP,
    LAN_SESSION_PROTOCOL_WEBSOCKET
} lan_session_protocol_t;

typedef struct st_lan_session_outbound
{
    uint8_t const * p_data;
    uint32_t data_bytes;
    uint8_t link_id;
    bool control_response;
} lan_session_outbound_t;

typedef struct st_lan_session_router_diagnostics
{
    uint32_t links_opened;
    uint32_t links_closed;
    uint32_t protocol_rejections;
    uint32_t http_requests;
    uint32_t websocket_upgrades;
    uint32_t websocket_frames;
    uint32_t websocket_errors;
    uint32_t telemetry_queued;
    uint32_t telemetry_replaced;
    uint32_t observer_commands_rejected;
    uint32_t controller_commands_rejected;
    uint32_t lease_acquisitions;
    uint32_t lease_releases;
    uint32_t output_bytes;
    uint8_t open_links;
    uint8_t websocket_links;
    uint8_t controller_link_id;
    uint8_t round_robin_link_id;
    bool web_asset_valid;
    app_status_t web_asset_status;
    app_status_t last_status;
    local_tcp_session_diagnostics_t legacy;
} lan_session_router_diagnostics_t;

void lan_session_router_init(void);
app_status_t lan_session_router_open_link(uint8_t link_id);
void lan_session_router_close_link(uint8_t link_id);
app_status_t lan_session_router_feed(uint8_t link_id,
                                     uint8_t const * p_data,
                                     uint32_t data_bytes);
app_status_t lan_session_router_queue_telemetry(
    app_telemetry_snapshot_t const * p_snapshot);
app_status_t lan_session_router_peek_outbound(lan_session_outbound_t * p_outbound);
app_status_t lan_session_router_mark_outbound_in_flight(uint8_t link_id,
                                                       bool control_response);
void lan_session_router_consume_outbound(uint8_t link_id,
                                         bool control_response,
                                         bool sent);
bool lan_session_router_should_close(uint8_t link_id);
void lan_session_router_get_diagnostics(lan_session_router_diagnostics_t * p_diagnostics);

#endif /* LAN_SESSION_ROUTER_H_ */

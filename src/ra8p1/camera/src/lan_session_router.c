#include "lan_session_router.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "http_protocol.h"
#include "web_ui_asset.h"
#include "websocket_protocol.h"

#define LAN_SESSION_DETECT_BYTES       (4U)
#define LAN_SESSION_OUTPUT_BYTES       (WEBSOCKET_PROTOCOL_MAX_FRAME_BYTES + 256U)
#define LAN_SESSION_JSON_BYTES         (WEBSOCKET_PROTOCOL_MAX_PAYLOAD_BYTES)
#define LAN_SESSION_HTTP_CHUNK_BYTES   (1024U)

typedef struct st_lan_output_buffer
{
    uint8_t bytes[LAN_SESSION_OUTPUT_BYTES];
    uint32_t used;
    bool ready;
    bool in_flight;
    bool state_snapshot;
} lan_output_buffer_t;

typedef struct st_lan_session
{
    lan_session_protocol_t protocol;
    http_protocol_parser_t http;
    websocket_protocol_decoder_t websocket;
    lan_output_buffer_t control;
    lan_output_buffer_t telemetry;
    uint8_t detect[LAN_SESSION_DETECT_BYTES];
    uint8_t detect_used;
    uint8_t const * p_http_body;
    uint32_t http_body_bytes;
    uint32_t http_body_offset;
    bool http_close_after_send;
    bool close_requested;
    bool controller;
    bool state_snapshot_pending;
    bool telemetry_pending;
} lan_session_t;

static lan_session_t g_sessions[LAN_SESSION_ROUTER_MAX_LINKS];
static lan_session_router_diagnostics_t g_diagnostics;
static app_telemetry_snapshot_t g_latest_snapshot;
static bool g_latest_snapshot_valid;
static web_ui_asset_info_t g_web_asset;

static app_status_t lan_session_feed_detect(uint8_t link_id,
                                            uint8_t const * p_data,
                                            uint32_t data_bytes,
                                            uint32_t * p_consumed_bytes);
static app_status_t lan_session_feed_http(uint8_t link_id,
                                          uint8_t const * p_data,
                                          uint32_t data_bytes);
static app_status_t lan_session_handle_http_request(uint8_t link_id,
                                                    http_protocol_request_t const * p_request);
static app_status_t lan_session_websocket_handler(
    websocket_protocol_frame_t const * p_frame,
    void * p_context);
static app_status_t lan_session_queue_control_json(uint8_t link_id,
                                                   char const * p_json);
static app_status_t lan_session_queue_websocket(uint8_t link_id,
                                                lan_output_buffer_t * p_output,
                                                websocket_protocol_opcode_t opcode,
                                                uint8_t const * p_payload,
                                                uint32_t payload_bytes);
static app_status_t lan_session_queue_http(uint8_t link_id,
                                           char const * p_status,
                                           char const * p_content_type,
                                           uint8_t const * p_body,
                                           uint32_t body_bytes,
                                           bool stream_body);
static app_status_t lan_session_queue_next_http_body(uint8_t link_id);
static app_status_t lan_session_queue_snapshot(uint8_t link_id,
                                               bool initial_snapshot);
static app_status_t lan_session_format_health(char * p_output,
                                              uint32_t output_bytes,
                                              uint32_t * p_used);
static app_status_t lan_session_record(app_status_t status);

void lan_session_router_init (void)
{
    uint8_t link_id;

    (void) memset(g_sessions, 0, sizeof(g_sessions));
    g_diagnostics = (lan_session_router_diagnostics_t) {0};
    g_diagnostics.controller_link_id = LAN_SESSION_ROUTER_NO_LINK;
    g_diagnostics.round_robin_link_id = 0U;
    g_latest_snapshot = (app_telemetry_snapshot_t) {0};
    g_latest_snapshot_valid = false;
    local_tcp_session_init();
    for (link_id = 0U; link_id < LAN_SESSION_ROUTER_MAX_LINKS; link_id++)
    {
        http_protocol_parser_init(&g_sessions[link_id].http);
        websocket_protocol_decoder_init(&g_sessions[link_id].websocket);
    }
    g_diagnostics.web_asset_status = web_ui_asset_init();
    web_ui_asset_get_info(&g_web_asset);
    g_diagnostics.web_asset_valid = g_web_asset.ospi_asset_valid;
    g_diagnostics.last_status = APP_STATUS_OK;
}

app_status_t lan_session_router_open_link (uint8_t link_id)
{
    lan_session_t * p_session;

    if (link_id >= LAN_SESSION_ROUTER_MAX_LINKS)
    {
        return lan_session_record(APP_STATUS_INVALID_ARGUMENT);
    }
    p_session = &g_sessions[link_id];
    if (LAN_SESSION_PROTOCOL_FREE != p_session->protocol)
    {
        return lan_session_record(APP_STATUS_OK);
    }
    *p_session = (lan_session_t) {0};
    p_session->protocol = LAN_SESSION_PROTOCOL_DETECT;
    http_protocol_parser_init(&p_session->http);
    websocket_protocol_decoder_init(&p_session->websocket);
    g_diagnostics.links_opened++;
    g_diagnostics.open_links++;
    return lan_session_record(APP_STATUS_OK);
}

void lan_session_router_close_link (uint8_t link_id)
{
    lan_session_t * p_session;

    if (link_id >= LAN_SESSION_ROUTER_MAX_LINKS)
    {
        return;
    }
    p_session = &g_sessions[link_id];
    if (LAN_SESSION_PROTOCOL_FREE == p_session->protocol)
    {
        return;
    }
    if (LAN_SESSION_PROTOCOL_LEGACY == p_session->protocol)
    {
        local_tcp_session_close_link(link_id);
    }
    if (LAN_SESSION_PROTOCOL_WEBSOCKET == p_session->protocol)
    {
        if (0U < g_diagnostics.websocket_links)
        {
            g_diagnostics.websocket_links--;
        }
        if (p_session->controller)
        {
            g_diagnostics.controller_link_id = LAN_SESSION_ROUTER_NO_LINK;
            g_diagnostics.lease_releases++;
        }
    }
    *p_session = (lan_session_t) {0};
    g_diagnostics.links_closed++;
    if (0U < g_diagnostics.open_links)
    {
        g_diagnostics.open_links--;
    }
    local_tcp_session_get_diagnostics(&g_diagnostics.legacy);
}

app_status_t lan_session_router_feed (uint8_t link_id,
                                      uint8_t const * p_data,
                                      uint32_t data_bytes)
{
    lan_session_t * p_session;
    app_status_t status = APP_STATUS_OK;
    uint32_t consumed_bytes = 0U;

    if ((link_id >= LAN_SESSION_ROUTER_MAX_LINKS) ||
        ((0U < data_bytes) && (NULL == p_data)))
    {
        return lan_session_record(APP_STATUS_INVALID_ARGUMENT);
    }
    p_session = &g_sessions[link_id];
    if (LAN_SESSION_PROTOCOL_FREE == p_session->protocol)
    {
        return lan_session_record(APP_STATUS_UNAVAILABLE);
    }
    if (LAN_SESSION_PROTOCOL_DETECT == p_session->protocol)
    {
        status = lan_session_feed_detect(link_id, p_data, data_bytes, &consumed_bytes);
        if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
        {
            g_diagnostics.protocol_rejections++;
            return lan_session_record(status);
        }
        if (LAN_SESSION_PROTOCOL_DETECT == p_session->protocol)
        {
            return lan_session_record(APP_STATUS_OK);
        }
    }

    if (LAN_SESSION_PROTOCOL_LEGACY == p_session->protocol)
    {
        status = local_tcp_session_feed(link_id,
                                        &p_data[consumed_bytes],
                                        data_bytes - consumed_bytes);
        local_tcp_session_get_diagnostics(&g_diagnostics.legacy);
    }
    else if (LAN_SESSION_PROTOCOL_HTTP == p_session->protocol)
    {
        status = lan_session_feed_http(link_id,
                                       &p_data[consumed_bytes],
                                       data_bytes - consumed_bytes);
    }
    else if (LAN_SESSION_PROTOCOL_WEBSOCKET == p_session->protocol)
    {
        status = websocket_protocol_decoder_feed(&p_session->websocket,
                                                  &p_data[consumed_bytes],
                                                  data_bytes - consumed_bytes,
                                                  lan_session_websocket_handler,
                                                  (void *) (uintptr_t) link_id);
        if ((APP_STATUS_OK == status) || (APP_STATUS_BUSY == status))
        {
            status = APP_STATUS_OK;
        }
        else
        {
            g_diagnostics.websocket_errors++;
        }
    }
    return lan_session_record(status);
}

app_status_t lan_session_router_queue_telemetry (
    app_telemetry_snapshot_t const * p_snapshot)
{
    uint8_t link_id;
    app_status_t overall_status;

    if (NULL == p_snapshot)
    {
        return lan_session_record(APP_STATUS_INVALID_ARGUMENT);
    }
    g_latest_snapshot = *p_snapshot;
    g_latest_snapshot_valid = true;
    overall_status = local_tcp_session_queue_telemetry(p_snapshot);
    local_tcp_session_get_diagnostics(&g_diagnostics.legacy);
    if ((APP_STATUS_OK != overall_status) && (APP_STATUS_BUSY != overall_status))
    {
        return lan_session_record(overall_status);
    }
    overall_status = APP_STATUS_OK;
    for (link_id = 0U; link_id < LAN_SESSION_ROUTER_MAX_LINKS; link_id++)
    {
        if (LAN_SESSION_PROTOCOL_WEBSOCKET == g_sessions[link_id].protocol)
        {
            app_status_t const status = lan_session_queue_snapshot(link_id, false);

            if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
            {
                overall_status = status;
            }
        }
    }
    return lan_session_record(overall_status);
}

app_status_t lan_session_router_peek_outbound (lan_session_outbound_t * p_outbound)
{
    uint8_t pass;

    if (NULL == p_outbound)
    {
        return lan_session_record(APP_STATUS_INVALID_ARGUMENT);
    }
    *p_outbound = (lan_session_outbound_t) {0};
    for (pass = 0U; pass < 2U; pass++)
    {
        uint8_t offset;

        for (offset = 0U; offset < LAN_SESSION_ROUTER_MAX_LINKS; offset++)
        {
            uint8_t const link_id = (uint8_t)
                ((g_diagnostics.round_robin_link_id + offset) % LAN_SESSION_ROUTER_MAX_LINKS);
            lan_session_t * const p_session = &g_sessions[link_id];
            lan_output_buffer_t * const p_output =
                (0U == pass) ? &p_session->control : &p_session->telemetry;

            if (LAN_SESSION_PROTOCOL_LEGACY == p_session->protocol)
            {
                local_tcp_outbound_t legacy;
                app_status_t const status = local_tcp_session_peek_outbound(&legacy);

                if ((APP_STATUS_OK == status) &&
                    (((0U == pass) && legacy.control_response) ||
                     ((1U == pass) && !legacy.control_response)))
                {
                    p_outbound->p_data = legacy.p_data;
                    p_outbound->data_bytes = legacy.data_bytes;
                    p_outbound->link_id = link_id;
                    p_outbound->control_response = legacy.control_response;
                    return lan_session_record(APP_STATUS_OK);
                }
            }
            else if (p_output->ready && !p_output->in_flight)
            {
                p_outbound->p_data = p_output->bytes;
                p_outbound->data_bytes = p_output->used;
                p_outbound->link_id = link_id;
                p_outbound->control_response = (0U == pass);
                return lan_session_record(APP_STATUS_OK);
            }
        }
    }
    return lan_session_record(APP_STATUS_UNAVAILABLE);
}

app_status_t lan_session_router_mark_outbound_in_flight (uint8_t link_id,
                                                        bool control_response)
{
    lan_session_t * p_session;
    lan_output_buffer_t * p_output;

    if (link_id >= LAN_SESSION_ROUTER_MAX_LINKS)
    {
        return lan_session_record(APP_STATUS_INVALID_ARGUMENT);
    }
    p_session = &g_sessions[link_id];
    if (LAN_SESSION_PROTOCOL_LEGACY == p_session->protocol)
    {
        return lan_session_record(local_tcp_session_mark_outbound_in_flight(control_response));
    }
    p_output = control_response ? &p_session->control : &p_session->telemetry;
    if (!p_output->ready)
    {
        return lan_session_record(APP_STATUS_UNAVAILABLE);
    }
    if (p_output->in_flight)
    {
        return lan_session_record(APP_STATUS_BUSY);
    }
    p_output->in_flight = true;
    return lan_session_record(APP_STATUS_OK);
}

void lan_session_router_consume_outbound (uint8_t link_id,
                                          bool control_response,
                                          bool sent)
{
    lan_session_t * p_session;
    lan_output_buffer_t * p_output;

    if (link_id >= LAN_SESSION_ROUTER_MAX_LINKS)
    {
        return;
    }
    p_session = &g_sessions[link_id];
    if (LAN_SESSION_PROTOCOL_LEGACY == p_session->protocol)
    {
        local_tcp_session_consume_outbound(control_response, sent);
        local_tcp_session_get_diagnostics(&g_diagnostics.legacy);
        g_diagnostics.round_robin_link_id = (uint8_t)
            ((link_id + 1U) % LAN_SESSION_ROUTER_MAX_LINKS);
        return;
    }
    p_output = control_response ? &p_session->control : &p_session->telemetry;
    if (!p_output->ready || !p_output->in_flight)
    {
        return;
    }
    bool const was_state_snapshot = p_output->state_snapshot;
    if (sent)
    {
        g_diagnostics.output_bytes += p_output->used;
        if (!control_response && was_state_snapshot)
        {
            p_session->state_snapshot_pending = false;
        }
    }
    *p_output = (lan_output_buffer_t) {0};
    g_diagnostics.round_robin_link_id = (uint8_t)
        ((link_id + 1U) % LAN_SESSION_ROUTER_MAX_LINKS);
    if (!sent)
    {
        p_session->close_requested = true;
    }
    else if (control_response && (p_session->http_body_offset < p_session->http_body_bytes))
    {
        if (APP_STATUS_OK != lan_session_queue_next_http_body(link_id))
        {
            p_session->close_requested = true;
        }
    }
    else if (control_response && p_session->http_close_after_send)
    {
        p_session->close_requested = true;
    }
    else if (!control_response && p_session->telemetry_pending &&
             g_latest_snapshot_valid &&
             (LAN_SESSION_PROTOCOL_WEBSOCKET == p_session->protocol))
    {
        p_session->telemetry_pending = false;
        (void) lan_session_queue_snapshot(link_id, false);
    }
    if (control_response && sent &&
        (LAN_SESSION_PROTOCOL_WEBSOCKET == p_session->protocol) &&
        (0U < p_session->websocket.used) &&
        !p_session->control.ready && !p_session->close_requested)
    {
        app_status_t const status = websocket_protocol_decoder_feed(
            &p_session->websocket,
            NULL,
            0U,
            lan_session_websocket_handler,
            (void *) (uintptr_t) link_id);

        if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
        {
            g_diagnostics.websocket_errors++;
            p_session->close_requested = true;
        }
    }
}

bool lan_session_router_should_close (uint8_t link_id)
{
    return (link_id < LAN_SESSION_ROUTER_MAX_LINKS) &&
           g_sessions[link_id].close_requested &&
           !g_sessions[link_id].control.in_flight &&
           !g_sessions[link_id].control.ready;
}

void lan_session_router_get_diagnostics (lan_session_router_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        local_tcp_session_get_diagnostics(&g_diagnostics.legacy);
        *p_diagnostics = g_diagnostics;
    }
}

static app_status_t lan_session_feed_detect (uint8_t link_id,
                                             uint8_t const * p_data,
                                             uint32_t data_bytes,
                                             uint32_t * p_consumed_bytes)
{
    lan_session_t * const p_session = &g_sessions[link_id];

    while ((p_session->detect_used < LAN_SESSION_DETECT_BYTES) &&
           (*p_consumed_bytes < data_bytes))
    {
        p_session->detect[p_session->detect_used++] = p_data[*p_consumed_bytes];
        (*p_consumed_bytes)++;
    }
    if (p_session->detect_used < LAN_SESSION_DETECT_BYTES)
    {
        return APP_STATUS_BUSY;
    }
    if (0 == memcmp(p_session->detect, "RA8P", LAN_SESSION_DETECT_BYTES))
    {
        app_status_t const status = local_tcp_session_open_link(link_id);

        if (APP_STATUS_OK != status)
        {
            return status;
        }
        p_session->protocol = LAN_SESSION_PROTOCOL_LEGACY;
        return local_tcp_session_feed(link_id,
                                      p_session->detect,
                                      LAN_SESSION_DETECT_BYTES);
    }
    if (0 == memcmp(p_session->detect, "GET ", LAN_SESSION_DETECT_BYTES))
    {
        p_session->protocol = LAN_SESSION_PROTOCOL_HTTP;
        return lan_session_feed_http(link_id,
                                     p_session->detect,
                                     LAN_SESSION_DETECT_BYTES);
    }
    return APP_STATUS_PROTOCOL;
}

static app_status_t lan_session_feed_http (uint8_t link_id,
                                           uint8_t const * p_data,
                                           uint32_t data_bytes)
{
    lan_session_t * const p_session = &g_sessions[link_id];
    http_protocol_request_t request;
    uint32_t consumed_bytes = 0U;
    app_status_t status = http_protocol_parser_feed(&p_session->http,
                                                    p_data,
                                                    data_bytes,
                                                    &consumed_bytes,
                                                    &request);

    if (APP_STATUS_BUSY == status)
    {
        return APP_STATUS_OK;
    }
    if (APP_STATUS_OK != status)
    {
        return status;
    }
    if (consumed_bytes != data_bytes)
    {
        return APP_STATUS_PROTOCOL;
    }
    g_diagnostics.http_requests++;
    return lan_session_handle_http_request(link_id, &request);
}

static app_status_t lan_session_handle_http_request (
    uint8_t link_id,
    http_protocol_request_t const * p_request)
{
    lan_session_t * const p_session = &g_sessions[link_id];

    if (HTTP_PROTOCOL_ROUTE_ROOT == p_request->route)
    {
        return lan_session_queue_http(link_id,
                                      "200 OK",
                                      "text/html; charset=utf-8",
                                      g_web_asset.p_page,
                                      g_web_asset.page_bytes,
                                      true);
    }
    if (HTTP_PROTOCOL_ROUTE_HEALTH == p_request->route)
    {
        char health[512];
        uint32_t health_bytes = 0U;
        app_status_t const status = lan_session_format_health(health,
                                                              sizeof(health),
                                                              &health_bytes);

        if (APP_STATUS_OK != status)
        {
            return status;
        }
        return lan_session_queue_http(link_id,
                                      "200 OK",
                                      "application/json; charset=utf-8",
                                      (uint8_t const *) health,
                                      health_bytes,
                                      false);
    }
    if ((HTTP_PROTOCOL_ROUTE_WEBSOCKET == p_request->route) &&
        p_request->websocket_upgrade)
    {
        char accept[WEBSOCKET_PROTOCOL_ACCEPT_KEY_BYTES];
        int header_bytes;
        app_status_t status = websocket_protocol_accept_key(p_request->websocket_key, accept);

        if (APP_STATUS_OK != status)
        {
            return status;
        }
        header_bytes = snprintf((char *) p_session->control.bytes,
                                sizeof(p_session->control.bytes),
                                "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: %s\r\n\r\n",
                                accept);
        if ((header_bytes < 0) || ((size_t) header_bytes >= sizeof(p_session->control.bytes)))
        {
            return APP_STATUS_DATA;
        }
        p_session->control.used = (uint32_t) header_bytes;
        p_session->control.ready = true;
        p_session->protocol = LAN_SESSION_PROTOCOL_WEBSOCKET;
        websocket_protocol_decoder_init(&p_session->websocket);
        p_session->state_snapshot_pending = true;
        g_diagnostics.websocket_links++;
        g_diagnostics.websocket_upgrades++;
        if (LAN_SESSION_ROUTER_NO_LINK == g_diagnostics.controller_link_id)
        {
            p_session->controller = true;
            g_diagnostics.controller_link_id = link_id;
            g_diagnostics.lease_acquisitions++;
        }
        if (g_latest_snapshot_valid)
        {
            status = lan_session_queue_snapshot(link_id, true);
        }
        return status;
    }
    return APP_STATUS_PROTOCOL;
}

static app_status_t lan_session_websocket_handler (
    websocket_protocol_frame_t const * p_frame,
    void * p_context)
{
    uint8_t const link_id = (uint8_t) (uintptr_t) p_context;
    lan_session_t * const p_session = &g_sessions[link_id];

    g_diagnostics.websocket_frames++;
    if (WEBSOCKET_PROTOCOL_OPCODE_PING == p_frame->opcode)
    {
        return lan_session_queue_websocket(link_id,
                                           &p_session->control,
                                           WEBSOCKET_PROTOCOL_OPCODE_PONG,
                                           p_frame->p_payload,
                                           p_frame->payload_bytes);
    }
    if (WEBSOCKET_PROTOCOL_OPCODE_CLOSE == p_frame->opcode)
    {
        p_session->close_requested = true;
        return lan_session_queue_websocket(link_id,
                                           &p_session->control,
                                           WEBSOCKET_PROTOCOL_OPCODE_CLOSE,
                                           p_frame->p_payload,
                                           p_frame->payload_bytes);
    }
    if (WEBSOCKET_PROTOCOL_OPCODE_TEXT == p_frame->opcode)
    {
        app_status_t status;

        if (!p_session->controller)
        {
            status = lan_session_queue_control_json(
                link_id,
                "{\"type\":\"command_result\",\"status\":\"read_only\","
                "\"message\":\"当前连接是观察端，不能下发任务\"}");
            if (APP_STATUS_OK == status)
            {
                g_diagnostics.observer_commands_rejected++;
            }
            return status;
        }
        status = lan_session_queue_control_json(
            link_id,
            "{\"type\":\"command_result\",\"status\":\"unsupported\","
            "\"message\":\"运动固件尚未安装，任务未执行\"}");
        if (APP_STATUS_OK == status)
        {
            g_diagnostics.controller_commands_rejected++;
        }
        return status;
    }
    return APP_STATUS_OK;
}

static app_status_t lan_session_queue_control_json (uint8_t link_id,
                                                    char const * p_json)
{
    return lan_session_queue_websocket(link_id,
                                       &g_sessions[link_id].control,
                                       WEBSOCKET_PROTOCOL_OPCODE_TEXT,
                                       (uint8_t const *) p_json,
                                       (uint32_t) strlen(p_json));
}

static app_status_t lan_session_queue_websocket (uint8_t link_id,
                                                 lan_output_buffer_t * p_output,
                                                 websocket_protocol_opcode_t opcode,
                                                 uint8_t const * p_payload,
                                                 uint32_t payload_bytes)
{
    app_status_t status;

    (void) link_id;
    if (p_output->ready || p_output->in_flight)
    {
        return APP_STATUS_BUSY;
    }
    status = websocket_protocol_encode(opcode,
                                       p_payload,
                                       payload_bytes,
                                       p_output->bytes,
                                       sizeof(p_output->bytes),
                                       &p_output->used);
    if (APP_STATUS_OK == status)
    {
        p_output->ready = true;
    }
    return status;
}

static app_status_t lan_session_queue_http (uint8_t link_id,
                                            char const * p_status,
                                            char const * p_content_type,
                                            uint8_t const * p_body,
                                            uint32_t body_bytes,
                                            bool stream_body)
{
    lan_session_t * const p_session = &g_sessions[link_id];
    int header_bytes;

    if (p_session->control.ready || p_session->control.in_flight)
    {
        return APP_STATUS_BUSY;
    }
    header_bytes = snprintf((char *) p_session->control.bytes,
                            sizeof(p_session->control.bytes),
                            "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
                            "Content-Length: %" PRIu32 "\r\n"
                            "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                            p_status,
                            p_content_type,
                            body_bytes);
    if ((header_bytes < 0) || ((size_t) header_bytes >= sizeof(p_session->control.bytes)))
    {
        return APP_STATUS_DATA;
    }
    p_session->control.used = (uint32_t) header_bytes;
    if (!stream_body)
    {
        if ((p_session->control.used + body_bytes) > sizeof(p_session->control.bytes))
        {
            return APP_STATUS_DATA;
        }
        (void) memcpy(&p_session->control.bytes[p_session->control.used], p_body, body_bytes);
        p_session->control.used += body_bytes;
        p_session->http_body_bytes = 0U;
        p_session->http_body_offset = 0U;
    }
    else
    {
        p_session->p_http_body = p_body;
        p_session->http_body_bytes = body_bytes;
        p_session->http_body_offset = 0U;
    }
    p_session->control.ready = true;
    p_session->http_close_after_send = true;
    return APP_STATUS_OK;
}

static app_status_t lan_session_queue_next_http_body (uint8_t link_id)
{
    lan_session_t * const p_session = &g_sessions[link_id];
    uint32_t chunk_bytes = p_session->http_body_bytes - p_session->http_body_offset;

    if (chunk_bytes > LAN_SESSION_HTTP_CHUNK_BYTES)
    {
        chunk_bytes = LAN_SESSION_HTTP_CHUNK_BYTES;
    }
    if ((0U == chunk_bytes) || (NULL == p_session->p_http_body))
    {
        return APP_STATUS_DATA;
    }
    (void) memcpy(p_session->control.bytes,
                  &p_session->p_http_body[p_session->http_body_offset],
                  chunk_bytes);
    p_session->control.used = chunk_bytes;
    p_session->control.ready = true;
    p_session->http_body_offset += chunk_bytes;
    return APP_STATUS_OK;
}

static app_status_t lan_session_queue_snapshot (uint8_t link_id,
                                                 bool initial_snapshot)
{
    lan_session_t * const p_session = &g_sessions[link_id];
    char telemetry[LAN_SESSION_JSON_BYTES];
    char payload[LAN_SESSION_JSON_BYTES];
    uint32_t telemetry_bytes = 0U;
    int payload_bytes;
    bool const replaced = p_session->telemetry.ready;
    bool const state_snapshot = initial_snapshot || p_session->state_snapshot_pending;
    app_status_t status;

    if (p_session->telemetry.in_flight)
    {
        p_session->telemetry_pending = true;
        g_diagnostics.telemetry_replaced++;
        return APP_STATUS_OK;
    }
    status = local_tcp_session_format_telemetry_json(
        telemetry,
        sizeof(telemetry),
        &g_latest_snapshot,
        &telemetry_bytes);

    if (APP_STATUS_OK != status)
    {
        return status;
    }
    if (state_snapshot)
    {
        payload_bytes = snprintf(payload,
                                 sizeof(payload),
                                 "{\"type\":\"state_snapshot\",\"control\":%s,\"data\":%s}",
                                 p_session->controller ? "true" : "false",
                                 telemetry);
    }
    else
    {
        payload_bytes = snprintf(payload,
                                 sizeof(payload),
                                 "{\"type\":\"telemetry\",\"data\":%s}",
                                 telemetry);
    }
    if ((payload_bytes < 0) || ((size_t) payload_bytes >= sizeof(payload)))
    {
        return APP_STATUS_DATA;
    }
    if (replaced)
    {
        p_session->telemetry = (lan_output_buffer_t) {0};
    }
    status = lan_session_queue_websocket(link_id,
                                         &p_session->telemetry,
                                         WEBSOCKET_PROTOCOL_OPCODE_TEXT,
                                         (uint8_t const *) payload,
                                         (uint32_t) payload_bytes);
    if (APP_STATUS_OK == status)
    {
        p_session->telemetry.state_snapshot = state_snapshot;
        g_diagnostics.telemetry_queued++;
        if (replaced)
        {
            g_diagnostics.telemetry_replaced++;
        }
    }
    return status;
}

static app_status_t lan_session_format_health (char * p_output,
                                               uint32_t output_bytes,
                                               uint32_t * p_used)
{
    int used;

    if ((NULL == p_output) || (NULL == p_used))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (g_latest_snapshot_valid)
    {
        used = snprintf(p_output,
                        output_bytes,
                        "{\"status\":\"ok\",\"sequence\":%" PRIu32
                        ",\"camera_state\":%u,\"ai_state\":%u,\"sd_state\":%u,"
                        "\"network_state\":%u,\"websocket_clients\":%u,"
                        "\"controller_link\":%u,\"web_asset_valid\":%s}",
                        g_latest_snapshot.sequence,
                        (unsigned int) g_latest_snapshot.camera_state,
                        (unsigned int) g_latest_snapshot.ai_state,
                        (unsigned int) g_latest_snapshot.sd_state,
                        (unsigned int) g_latest_snapshot.network_state,
                        (unsigned int) g_diagnostics.websocket_links,
                        (unsigned int) g_diagnostics.controller_link_id,
                        g_diagnostics.web_asset_valid ? "true" : "false");
    }
    else
    {
        used = snprintf(p_output,
                        output_bytes,
                        "{\"status\":\"starting\",\"websocket_clients\":%u,"
                        "\"controller_link\":%u,\"web_asset_valid\":%s}",
                        (unsigned int) g_diagnostics.websocket_links,
                        (unsigned int) g_diagnostics.controller_link_id,
                        g_diagnostics.web_asset_valid ? "true" : "false");
    }
    if ((used < 0) || ((uint32_t) used >= output_bytes))
    {
        return APP_STATUS_DATA;
    }
    *p_used = (uint32_t) used;
    return APP_STATUS_OK;
}

static app_status_t lan_session_record (app_status_t status)
{
    g_diagnostics.last_status = status;
    return status;
}

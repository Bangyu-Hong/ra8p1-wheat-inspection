#ifndef LOCAL_TCP_SESSION_H_
#define LOCAL_TCP_SESSION_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "app_telemetry.h"

#define LOCAL_TCP_SESSION_NO_LINK  (0xFFU)
#define LOCAL_TCP_SESSION_CONTROL_RESPONSES  (4U)

typedef struct st_local_tcp_outbound
{
    uint8_t const * p_data;
    uint32_t data_bytes;
    bool control_response;
} local_tcp_outbound_t;

typedef struct st_local_tcp_session_diagnostics
{
    uint32_t links_accepted;
    uint32_t links_rejected;
    uint32_t links_closed;
    uint32_t bytes_received;
    uint32_t frames_received;
    uint32_t hello_requests;
    uint32_t ping_requests;
    uint32_t unsupported_requests;
    uint32_t responses_queued;
    uint32_t response_queue_busy;
    uint32_t response_queue_high_water;
    uint32_t telemetry_queued;
    uint32_t telemetry_replaced;
    uint32_t telemetry_queue_busy;
    uint32_t telemetry_cached;
    uint32_t snapshots_queued;
    uint32_t snapshots_sent;
    uint32_t snapshot_format_errors;
    uint32_t snapshot_encode_errors;
    uint32_t protocol_errors;
    uint32_t output_bytes;
    uint8_t active_link_id;
    bool negotiated;
    bool snapshot_pending;
    app_status_t last_snapshot_status;
    app_status_t last_status;
} local_tcp_session_diagnostics_t;

void local_tcp_session_init(void);
app_status_t local_tcp_session_open_link(uint8_t link_id);
void local_tcp_session_close_link(uint8_t link_id);
app_status_t local_tcp_session_feed(uint8_t link_id,
                                    uint8_t const * p_data,
                                    uint32_t data_bytes);
app_status_t local_tcp_session_queue_telemetry(app_telemetry_snapshot_t const * p_snapshot);
app_status_t local_tcp_session_peek_outbound(local_tcp_outbound_t * p_outbound);
app_status_t local_tcp_session_mark_outbound_in_flight(bool control_response);
void local_tcp_session_consume_outbound(bool control_response, bool sent);
void local_tcp_session_get_diagnostics(local_tcp_session_diagnostics_t * p_diagnostics);
app_status_t local_tcp_session_format_telemetry_json(
    char * p_payload,
    uint32_t payload_capacity,
    app_telemetry_snapshot_t const * p_snapshot,
    uint32_t * p_payload_bytes);

#endif /* LOCAL_TCP_SESSION_H_ */

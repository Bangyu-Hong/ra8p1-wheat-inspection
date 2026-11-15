#include "local_tcp_session.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ra8p_wire_contract.h"
#include "wire_protocol.h"

typedef struct st_local_tcp_buffer
{
    uint8_t bytes[WIRE_PROTOCOL_MAX_FRAME_BYTES];
    uint32_t used;
    bool ready;
    bool in_flight;
    bool snapshot;
} local_tcp_buffer_t;

static uint8_t g_decoder_storage[WIRE_PROTOCOL_MAX_FRAME_BYTES];
static wire_protocol_decoder_t g_decoder;
static local_tcp_buffer_t g_control_outputs[LOCAL_TCP_SESSION_CONTROL_RESPONSES];
static uint32_t g_control_head;
static uint32_t g_control_tail;
static uint32_t g_control_used;
static local_tcp_buffer_t g_telemetry_output;
static app_telemetry_snapshot_t g_latest_snapshot;
static bool g_latest_snapshot_valid;
static local_tcp_session_diagnostics_t g_diagnostics;

static app_status_t local_tcp_session_handle_frame(wire_protocol_frame_t const * p_frame,
                                                   void * p_context);
static app_status_t local_tcp_session_queue_control(uint16_t type,
                                                     uint32_t sequence,
                                                     uint32_t flags,
                                                     char const * p_payload);
static app_status_t local_tcp_session_queue_snapshot(
    app_telemetry_snapshot_t const * p_snapshot,
    bool initial_snapshot);
static bool local_tcp_session_format_fixed(char * p_output,
                                           size_t output_bytes,
                                           int64_t value,
                                           uint32_t divisor,
                                           uint32_t fraction_digits);
static bool local_tcp_session_format_u64(char * p_output,
                                         size_t output_bytes,
                                         uint64_t value);
static bool local_tcp_session_string_terminated(char const * p_string,
                                                size_t string_bytes);
static app_status_t local_tcp_session_record(app_status_t status);

void local_tcp_session_init (void)
{
    (void) memset(g_control_outputs, 0, sizeof(g_control_outputs));
    g_control_head = 0U;
    g_control_tail = 0U;
    g_control_used = 0U;
    g_telemetry_output = (local_tcp_buffer_t) {0};
    g_latest_snapshot = (app_telemetry_snapshot_t) {0};
    g_latest_snapshot_valid = false;
    g_diagnostics = (local_tcp_session_diagnostics_t) {0};
    g_diagnostics.active_link_id = LOCAL_TCP_SESSION_NO_LINK;
    g_diagnostics.last_status = wire_protocol_decoder_init(&g_decoder,
                                                            g_decoder_storage,
                                                            sizeof(g_decoder_storage));
}

app_status_t local_tcp_session_open_link (uint8_t link_id)
{
    if (LOCAL_TCP_SESSION_NO_LINK == g_diagnostics.active_link_id)
    {
        wire_protocol_decoder_reset(&g_decoder);
        (void) memset(g_control_outputs, 0, sizeof(g_control_outputs));
        g_control_head = 0U;
        g_control_tail = 0U;
        g_control_used = 0U;
        g_telemetry_output = (local_tcp_buffer_t) {0};
        g_diagnostics.active_link_id = link_id;
        g_diagnostics.negotiated = false;
        g_diagnostics.snapshot_pending = false;
        g_diagnostics.links_accepted++;
        return local_tcp_session_record(APP_STATUS_OK);
    }
    if (g_diagnostics.active_link_id == link_id)
    {
        return local_tcp_session_record(APP_STATUS_OK);
    }

    g_diagnostics.links_rejected++;
    return local_tcp_session_record(APP_STATUS_BUSY);
}

void local_tcp_session_close_link (uint8_t link_id)
{
    if (g_diagnostics.active_link_id == link_id)
    {
        wire_protocol_decoder_reset(&g_decoder);
        (void) memset(g_control_outputs, 0, sizeof(g_control_outputs));
        g_control_head = 0U;
        g_control_tail = 0U;
        g_control_used = 0U;
        g_telemetry_output = (local_tcp_buffer_t) {0};
        g_diagnostics.active_link_id = LOCAL_TCP_SESSION_NO_LINK;
        g_diagnostics.negotiated = false;
        g_diagnostics.snapshot_pending = false;
        g_diagnostics.links_closed++;
    }
}

app_status_t local_tcp_session_feed (uint8_t link_id,
                                     uint8_t const * p_data,
                                     uint32_t data_bytes)
{
    uint32_t consumed_bytes = 0U;
    app_status_t status;

    if ((0U < data_bytes) && (NULL == p_data))
    {
        return local_tcp_session_record(APP_STATUS_INVALID_ARGUMENT);
    }
    if (g_diagnostics.active_link_id != link_id)
    {
        return local_tcp_session_record(APP_STATUS_UNAVAILABLE);
    }

    status = wire_protocol_decoder_feed(&g_decoder,
                                        p_data,
                                        data_bytes,
                                        &consumed_bytes,
                                        local_tcp_session_handle_frame,
                                        NULL);
    g_diagnostics.bytes_received += consumed_bytes;
    if ((APP_STATUS_BUSY == status) && (consumed_bytes == data_bytes))
    {
        return local_tcp_session_record(APP_STATUS_OK);
    }
    if ((APP_STATUS_OK != status) || (consumed_bytes != data_bytes))
    {
        g_diagnostics.protocol_errors++;
        wire_protocol_decoder_reset(&g_decoder);
        return local_tcp_session_record((APP_STATUS_OK == status) ? APP_STATUS_PROTOCOL : status);
    }

    return local_tcp_session_record(APP_STATUS_OK);
}

app_status_t local_tcp_session_queue_telemetry (app_telemetry_snapshot_t const * p_snapshot)
{
    if (NULL == p_snapshot)
    {
        return local_tcp_session_record(APP_STATUS_INVALID_ARGUMENT);
    }

    g_latest_snapshot = *p_snapshot;
    g_latest_snapshot_valid = true;
    g_diagnostics.telemetry_cached++;
    if ((LOCAL_TCP_SESSION_NO_LINK == g_diagnostics.active_link_id) ||
        !g_diagnostics.negotiated)
    {
        return local_tcp_session_record(APP_STATUS_OK);
    }

    return local_tcp_session_record(local_tcp_session_queue_snapshot(
        &g_latest_snapshot,
        g_diagnostics.snapshot_pending ||
        (g_telemetry_output.ready && g_telemetry_output.snapshot)));
}

static app_status_t local_tcp_session_queue_snapshot (
    app_telemetry_snapshot_t const * p_snapshot,
    bool initial_snapshot)
{
    char payload[1024];
    uint32_t payload_bytes = 0U;
    uint32_t frame_bytes = 0U;
    app_status_t status;

    if (NULL == p_snapshot)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (g_telemetry_output.in_flight)
    {
        g_diagnostics.telemetry_queue_busy++;
        return APP_STATUS_BUSY;
    }

    status = local_tcp_session_format_telemetry_json(payload,
                                                     sizeof(payload),
                                                     p_snapshot,
                                                     &payload_bytes);
    if (APP_STATUS_OK != status)
    {
        g_diagnostics.snapshot_format_errors++;
        g_diagnostics.last_snapshot_status = status;
        return status;
    }

    status = wire_protocol_encode(initial_snapshot ? RA8P_MESSAGE_STATE_SNAPSHOT :
                                                      RA8P_MESSAGE_TELEMETRY,
                                  p_snapshot->sequence,
                                  initial_snapshot ? RA8P_FLAG_SNAPSHOT : RA8P_FLAG_NONE,
                                  (uint8_t const *) payload,
                                  payload_bytes,
                                  g_telemetry_output.bytes,
                                  sizeof(g_telemetry_output.bytes),
                                  &frame_bytes);
    if (APP_STATUS_OK == status)
    {
        if (g_telemetry_output.ready)
        {
            g_diagnostics.telemetry_replaced++;
        }
        g_telemetry_output.used = frame_bytes;
        g_telemetry_output.ready = true;
        g_telemetry_output.snapshot = initial_snapshot;
        g_diagnostics.telemetry_queued++;
        if (initial_snapshot)
        {
            g_diagnostics.snapshots_queued++;
        }
    }
    else
    {
        g_diagnostics.snapshot_encode_errors++;
    }
    g_diagnostics.last_snapshot_status = status;
    return status;
}

app_status_t local_tcp_session_peek_outbound (local_tcp_outbound_t * p_outbound)
{
    if (NULL == p_outbound)
    {
        return local_tcp_session_record(APP_STATUS_INVALID_ARGUMENT);
    }
    *p_outbound = (local_tcp_outbound_t) {0};
    if (0U < g_control_used)
    {
        p_outbound->p_data = g_control_outputs[g_control_tail].bytes;
        p_outbound->data_bytes = g_control_outputs[g_control_tail].used;
        p_outbound->control_response = true;
        return local_tcp_session_record(APP_STATUS_OK);
    }
    if (g_telemetry_output.ready)
    {
        p_outbound->p_data = g_telemetry_output.bytes;
        p_outbound->data_bytes = g_telemetry_output.used;
        p_outbound->control_response = false;
        return local_tcp_session_record(APP_STATUS_OK);
    }
    return local_tcp_session_record(APP_STATUS_UNAVAILABLE);
}

app_status_t local_tcp_session_mark_outbound_in_flight (bool control_response)
{
    local_tcp_buffer_t * const p_output = control_response ?
                                      &g_control_outputs[g_control_tail] :
                                      &g_telemetry_output;

    if ((control_response && (0U == g_control_used)) || !p_output->ready)
    {
        return local_tcp_session_record(APP_STATUS_UNAVAILABLE);
    }
    if (p_output->in_flight)
    {
        return local_tcp_session_record(APP_STATUS_BUSY);
    }
    p_output->in_flight = true;
    return local_tcp_session_record(APP_STATUS_OK);
}

void local_tcp_session_consume_outbound (bool control_response, bool sent)
{
    local_tcp_buffer_t * p_output = control_response ?
                                  &g_control_outputs[g_control_tail] :
                                  &g_telemetry_output;

    if ((!control_response || (0U < g_control_used)) &&
        p_output->ready && p_output->in_flight)
    {
        bool const was_snapshot = p_output->snapshot;

        if (sent)
        {
            g_diagnostics.output_bytes += p_output->used;
            if (!control_response && was_snapshot)
            {
                g_diagnostics.snapshot_pending = false;
                g_diagnostics.snapshots_sent++;
            }
        }
        p_output->used = 0U;
        p_output->ready = false;
        p_output->in_flight = false;
        p_output->snapshot = false;
        if (control_response)
        {
            uint32_t consumed_bytes = 0U;
            app_status_t status;

            g_control_tail = (g_control_tail + 1U) %
                             LOCAL_TCP_SESSION_CONTROL_RESPONSES;
            g_control_used--;
            status = wire_protocol_decoder_feed(&g_decoder,
                                                NULL,
                                                0U,
                                                &consumed_bytes,
                                                local_tcp_session_handle_frame,
                                                NULL);

            if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
            {
                g_diagnostics.protocol_errors++;
                wire_protocol_decoder_reset(&g_decoder);
                (void) local_tcp_session_record(status);
            }
        }
        else if (sent && g_diagnostics.snapshot_pending && g_latest_snapshot_valid)
        {
            (void) local_tcp_session_queue_snapshot(&g_latest_snapshot, true);
        }
    }
}

void local_tcp_session_get_diagnostics (local_tcp_session_diagnostics_t * p_diagnostics)
{
    if (NULL != p_diagnostics)
    {
        *p_diagnostics = g_diagnostics;
    }
}

static app_status_t local_tcp_session_handle_frame (wire_protocol_frame_t const * p_frame,
                                                    void * p_context)
{
    char payload[256];
    int payload_bytes;

    (void) p_context;
    if (NULL == p_frame)
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (g_control_used >= LOCAL_TCP_SESSION_CONTROL_RESPONSES)
    {
        g_diagnostics.response_queue_busy++;
        return APP_STATUS_BUSY;
    }

    g_diagnostics.frames_received++;
    switch (p_frame->type)
    {
        case RA8P_MESSAGE_HELLO:
            g_diagnostics.hello_requests++;
            payload_bytes = snprintf(payload,
                                     sizeof(payload),
                                     "{\"build_version\":\"%s\",\"capabilities\":%" PRIu32 ","
                                     "\"device_id\":\"%s\",\"session_id\":\"%s\","
                                     "\"status\":0}",
                                     g_latest_snapshot_valid ?
                                         g_latest_snapshot.build_version : "fsp-6.5.0",
                                     g_latest_snapshot_valid ?
                                         g_latest_snapshot.capabilities :
                                         (uint32_t) (RA8P_CAPABILITY_ENVIRONMENT |
                                                     RA8P_CAPABILITY_VISION |
                                                     RA8P_CAPABILITY_LAN_TCP |
                                                     RA8P_CAPABILITY_MISSION_PLAN |
                                                     RA8P_CAPABILITY_AI_INFERENCE),
                                     g_latest_snapshot_valid ?
                                         g_latest_snapshot.device_id : "ra8p1-cpkhmi",
                                     g_latest_snapshot_valid ?
                                         g_latest_snapshot.session_id : "local-boot");
            if ((payload_bytes < 0) || ((size_t) payload_bytes >= sizeof(payload)))
            {
                return APP_STATUS_DATA;
            }
            {
                app_status_t const status = local_tcp_session_queue_control(
                    RA8P_MESSAGE_HELLO_ACK,
                    p_frame->sequence,
                    RA8P_FLAG_RESPONSE,
                    payload);

                if (APP_STATUS_OK == status)
                {
                    g_diagnostics.negotiated = true;
                    g_diagnostics.snapshot_pending = true;
                    if (g_latest_snapshot_valid && !g_telemetry_output.in_flight)
                    {
                        (void) local_tcp_session_queue_snapshot(&g_latest_snapshot, true);
                    }
                }
                return status;
            }

        case RA8P_MESSAGE_PING:
            g_diagnostics.ping_requests++;
            {
                app_status_t const status = wire_protocol_encode(
                    RA8P_MESSAGE_PONG,
                    p_frame->sequence,
                    RA8P_FLAG_RESPONSE,
                    p_frame->p_payload,
                    p_frame->payload_bytes,
                    g_control_outputs[g_control_head].bytes,
                    sizeof(g_control_outputs[g_control_head].bytes),
                    &g_control_outputs[g_control_head].used);

                if (APP_STATUS_OK == status)
                {
                    g_control_outputs[g_control_head].ready = true;
                    g_control_head = (g_control_head + 1U) %
                                     LOCAL_TCP_SESSION_CONTROL_RESPONSES;
                    g_control_used++;
                    g_diagnostics.responses_queued++;
                    if (g_control_used > g_diagnostics.response_queue_high_water)
                    {
                        g_diagnostics.response_queue_high_water = g_control_used;
                    }
                }
                return status;
            }

        case RA8P_MESSAGE_COMMAND:
            g_diagnostics.unsupported_requests++;
            return local_tcp_session_queue_control(
                RA8P_MESSAGE_COMMAND_ACK,
                p_frame->sequence,
                RA8P_FLAG_RESPONSE | RA8P_FLAG_ERROR,
                "{\"message\":\"Motion firmware is not installed\",\"status\":5}");

        case RA8P_MESSAGE_MISSION_VALIDATE:
        case RA8P_MESSAGE_MISSION_APPLY:
            g_diagnostics.unsupported_requests++;
            return local_tcp_session_queue_control(
                RA8P_MESSAGE_MISSION_STATUS,
                p_frame->sequence,
                RA8P_FLAG_RESPONSE | RA8P_FLAG_ERROR,
                "{\"issues\":[],\"message\":\"Motion firmware is not installed\",\"status\":5}");

        default:
            g_diagnostics.unsupported_requests++;
            return local_tcp_session_queue_control(
                RA8P_MESSAGE_ERROR,
                p_frame->sequence,
                RA8P_FLAG_RESPONSE | RA8P_FLAG_ERROR,
                "{\"message\":\"Unsupported message type\",\"status\":4}");
    }
}

static app_status_t local_tcp_session_queue_control (uint16_t type,
                                                     uint32_t sequence,
                                                     uint32_t flags,
                                                     char const * p_payload)
{
    app_status_t status;

    status = wire_protocol_encode(type,
                                  sequence,
                                  flags,
                                  (uint8_t const *) p_payload,
                                  (uint32_t) strlen(p_payload),
                                  g_control_outputs[g_control_head].bytes,
                                  sizeof(g_control_outputs[g_control_head].bytes),
                                  &g_control_outputs[g_control_head].used);
    if (APP_STATUS_OK == status)
    {
        g_control_outputs[g_control_head].ready = true;
        g_control_head = (g_control_head + 1U) %
                         LOCAL_TCP_SESSION_CONTROL_RESPONSES;
        g_control_used++;
        g_diagnostics.responses_queued++;
        if (g_control_used > g_diagnostics.response_queue_high_water)
        {
            g_diagnostics.response_queue_high_water = g_control_used;
        }
    }
    return status;
}

app_status_t local_tcp_session_format_telemetry_json (
    char * p_payload,
    uint32_t payload_capacity,
    app_telemetry_snapshot_t const * p_snapshot,
    uint32_t * p_payload_bytes)
{
    char temperature[24];
    char humidity[24];
    char inference[24];
    char light_percent[24];
    char monotonic_us[24];
    char wall_time_ms[24];
    int payload_bytes;

    if ((NULL == p_payload) || (0U == payload_capacity) ||
        (NULL == p_snapshot) || (NULL == p_payload_bytes))
    {
        return APP_STATUS_INVALID_ARGUMENT;
    }
    if (!local_tcp_session_string_terminated(p_snapshot->device_id,
                                             sizeof(p_snapshot->device_id)) ||
        !local_tcp_session_string_terminated(p_snapshot->session_id,
                                             sizeof(p_snapshot->session_id)) ||
        !local_tcp_session_string_terminated(p_snapshot->build_version,
                                             sizeof(p_snapshot->build_version)) ||
        !local_tcp_session_string_terminated(p_snapshot->model_version,
                                             sizeof(p_snapshot->model_version)) ||
        !local_tcp_session_format_fixed(temperature,
                                        sizeof(temperature),
                                        p_snapshot->temperature_milli_c,
                                        1000U,
                                        3U) ||
        !local_tcp_session_format_fixed(humidity,
                                        sizeof(humidity),
                                        p_snapshot->humidity_milli_percent,
                                        1000U,
                                        3U) ||
        !local_tcp_session_format_fixed(inference,
                                        sizeof(inference),
                                        p_snapshot->inference_us,
                                        1000U,
                                        3U) ||
        !local_tcp_session_format_fixed(light_percent,
                                        sizeof(light_percent),
                                        p_snapshot->light_tenths_percent,
                                        10U,
                                        1U) ||
        !local_tcp_session_format_u64(monotonic_us,
                                      sizeof(monotonic_us),
                                      p_snapshot->monotonic_us) ||
        !local_tcp_session_format_u64(wall_time_ms,
                                      sizeof(wall_time_ms),
                                      p_snapshot->wall_time_ms))
    {
        return APP_STATUS_DATA;
    }

    payload_bytes = snprintf(
        p_payload,
        payload_capacity,
        "{\"actual_position_mm\":%" PRId32 ",\"ai_state\":%u,"
        "\"build_version\":\"%s\",\"camera_state\":%u,"
        "\"capabilities\":%" PRIu32 ",\"device_id\":\"%s\","
        "\"dht11_fault_code\":%u,\"environment_age_ms\":%" PRIu32 ","
        "\"environment_valid\":%s,\"fault_flags\":%" PRIu32 ","
        "\"field_column\":%u,\"field_row\":%u,\"humidity_percent\":%s,"
        "\"mission_wheat_count\":%" PRIu32 ",\"field_wheat_count\":%" PRIu32 ","
        "\"counted_frame_count\":%" PRIu32 ","
        "\"inference_ms\":%s,\"light_average\":%u,"
        "\"light_calibration_version\":%u,\"light_fault_code\":%u,"
        "\"light_percent\":%s,\"light_raw\":%u,\"light_valid\":%s,"
        "\"mission_state\":%u,\"model_version\":\"%s\","
        "\"monotonic_us\":%s,\"network_state\":%u,"
        "\"region_status\":%u,\"schema_version\":%u,\"sd_state\":%u,"
        "\"sequence\":%" PRIu32 ",\"slide_position_index\":%u,"
        "\"session_id\":\"%s\",\"target_position_mm\":%" PRIu32 ","
        "\"temperature_c\":%s,\"time_valid\":%s,"
        "\"wall_time_ms\":%s,\"wheat_count\":%" PRId32 "}",
        p_snapshot->actual_position_mm,
        (unsigned int) p_snapshot->ai_state,
        p_snapshot->build_version,
        (unsigned int) p_snapshot->camera_state,
        p_snapshot->capabilities,
        p_snapshot->device_id,
        (unsigned int) p_snapshot->dht11_fault_code,
        p_snapshot->environment_age_ms,
        p_snapshot->environment_valid ? "true" : "false",
        p_snapshot->fault_flags,
        (unsigned int) p_snapshot->field_column,
        (unsigned int) p_snapshot->field_row,
        humidity,
        p_snapshot->mission_wheat_count,
        p_snapshot->field_wheat_count,
        p_snapshot->counted_frame_count,
        inference,
        (unsigned int) p_snapshot->light_average,
        (unsigned int) p_snapshot->light_calibration_version,
        (unsigned int) p_snapshot->light_fault_code,
        light_percent,
        (unsigned int) p_snapshot->light_raw,
        p_snapshot->light_valid ? "true" : "false",
        (unsigned int) p_snapshot->mission_state,
        p_snapshot->model_version,
        monotonic_us,
        (unsigned int) p_snapshot->network_state,
        (unsigned int) p_snapshot->region_status,
        (unsigned int) p_snapshot->schema_version,
        (unsigned int) p_snapshot->sd_state,
        p_snapshot->sequence,
        (unsigned int) p_snapshot->slide_position_index,
        p_snapshot->session_id,
        p_snapshot->target_position_mm,
        temperature,
        p_snapshot->wall_time_valid ? "true" : "false",
        wall_time_ms,
        p_snapshot->wheat_count);
    if ((payload_bytes < 0) || ((size_t) payload_bytes >= payload_capacity))
    {
        return APP_STATUS_DATA;
    }

    *p_payload_bytes = (uint32_t) payload_bytes;
    return APP_STATUS_OK;
}

static bool local_tcp_session_format_fixed (char * p_output,
                                            size_t output_bytes,
                                            int64_t value,
                                            uint32_t divisor,
                                            uint32_t fraction_digits)
{
    char whole_text[24];
    uint64_t magnitude;
    uint64_t whole;
    uint32_t fraction;
    int written;

    if ((NULL == p_output) || (0U == output_bytes) || (0U == divisor))
    {
        return false;
    }
    magnitude = (value < 0) ? (uint64_t) (-(value + 1)) + 1U : (uint64_t) value;
    whole = magnitude / divisor;
    fraction = (uint32_t) (magnitude % divisor);
    if (!local_tcp_session_format_u64(whole_text, sizeof(whole_text), whole))
    {
        return false;
    }
    written = snprintf(p_output,
                       output_bytes,
                       (value < 0) ? "-%s.%0*" PRIu32 : "%s.%0*" PRIu32,
                       whole_text,
                       (int) fraction_digits,
                       fraction);
    return (written >= 0) && ((size_t) written < output_bytes);
}

static bool local_tcp_session_format_u64 (char * p_output,
                                          size_t output_bytes,
                                          uint64_t value)
{
    char reversed[20];
    size_t digits = 0U;
    size_t index;

    if ((NULL == p_output) || (0U == output_bytes))
    {
        return false;
    }
    do
    {
        reversed[digits] = (char) ('0' + (char) (value % 10U));
        digits++;
        value /= 10U;
    } while ((0U != value) && (digits < sizeof(reversed)));

    if ((0U != value) || (digits >= output_bytes))
    {
        return false;
    }
    for (index = 0U; index < digits; index++)
    {
        p_output[index] = reversed[digits - index - 1U];
    }
    p_output[digits] = '\0';
    return true;
}

static bool local_tcp_session_string_terminated (char const * p_string,
                                                 size_t string_bytes)
{
    return (NULL != p_string) &&
           (NULL != memchr(p_string, '\0', string_bytes));
}

static app_status_t local_tcp_session_record (app_status_t status)
{
    g_diagnostics.last_status = status;
    return status;
}

#ifndef ESP_AT_SERVICE_H_
#define ESP_AT_SERVICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"
#include "app_telemetry.h"
#include "esp_at_parser.h"
#include "esp_at_transport.h"
#include "lan_session_router.h"

#define ESP_AT_SERVICE_TCP_PORT  (80U)

typedef enum e_esp_at_service_state
{
    ESP_AT_SERVICE_STATE_CLOSED = 0,
    ESP_AT_SERVICE_STATE_RESET_ASSERT,
    ESP_AT_SERVICE_STATE_RESET_HOLD,
    ESP_AT_SERVICE_STATE_RESET_RELEASE,
    ESP_AT_SERVICE_STATE_RESET_BOOT_WAIT,
    ESP_AT_SERVICE_STATE_RESET_FAULT,
    ESP_AT_SERVICE_STATE_POWER_WAIT,
    ESP_AT_SERVICE_STATE_PROBE_SEND,
    ESP_AT_SERVICE_STATE_PROBE_WAIT,
    ESP_AT_SERVICE_STATE_ECHO_SEND,
    ESP_AT_SERVICE_STATE_ECHO_WAIT,
    ESP_AT_SERVICE_STATE_VERSION_SEND,
    ESP_AT_SERVICE_STATE_VERSION_WAIT,
    ESP_AT_SERVICE_STATE_CREDENTIALS_WAIT,
    ESP_AT_SERVICE_STATE_WIFI_MODE_SEND,
    ESP_AT_SERVICE_STATE_WIFI_MODE_WAIT,
    ESP_AT_SERVICE_STATE_JOIN_SEND,
    ESP_AT_SERVICE_STATE_JOIN_WAIT,
    ESP_AT_SERVICE_STATE_IP_QUERY_SEND,
    ESP_AT_SERVICE_STATE_IP_QUERY_WAIT,
    ESP_AT_SERVICE_STATE_IP_QUERY_RETRY_WAIT,
    ESP_AT_SERVICE_STATE_MUX_SEND,
    ESP_AT_SERVICE_STATE_MUX_WAIT,
    ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_SEND,
    ESP_AT_SERVICE_STATE_MAX_CONNECTIONS_WAIT,
    ESP_AT_SERVICE_STATE_SERVER_STOP_SEND,
    ESP_AT_SERVICE_STATE_SERVER_STOP_WAIT,
    ESP_AT_SERVICE_STATE_SERVER_START_SEND,
    ESP_AT_SERVICE_STATE_SERVER_START_WAIT,
    ESP_AT_SERVICE_STATE_LISTENING,
    ESP_AT_SERVICE_STATE_BACKOFF
} esp_at_service_state_t;

typedef enum e_esp_at_network_state
{
    ESP_AT_NETWORK_OFFLINE = 0,
    ESP_AT_NETWORK_PROBING,
    ESP_AT_NETWORK_CREDENTIALS_WAIT,
    ESP_AT_NETWORK_JOINING,
    ESP_AT_NETWORK_LISTENING,
    ESP_AT_NETWORK_SESSION,
    ESP_AT_NETWORK_BACKOFF
} esp_at_network_state_t;

typedef struct st_esp_at_service_diagnostics
{
    esp_at_service_state_t state;
    esp_at_network_state_t network_state;
    uint32_t state_transitions;
    uint32_t commands_sent;
    uint32_t command_timeouts;
    uint32_t command_errors;
    uint32_t parser_errors;
    uint32_t recoveries;
    uint32_t reset_count;
    uint32_t reset_errors;
    uint32_t wifi_disconnects;
    uint32_t ip_query_retries;
    uint32_t links_opened;
    uint32_t links_rejected;
    uint32_t links_closed;
    uint32_t ipd_bytes;
    uint32_t ignored_ipd_bytes;
    uint32_t send_requests;
    uint32_t send_completions;
    uint32_t send_failures;
    uint32_t telemetry_busy;
    uint32_t consecutive_failures;
    uint8_t active_link_id;
    bool initialized;
    bool reset_asserted;
    bool credentials_configured;
    bool wifi_connected;
    bool got_ip;
    bool server_listening;
    bool send_in_progress;
    char station_ip[16];
    char firmware_version[96];
    app_status_t last_status;
    esp_at_parser_diagnostics_t parser;
    esp_at_transport_diagnostics_t transport;
    lan_session_router_diagnostics_t session;
} esp_at_service_diagnostics_t;

app_status_t esp_at_service_init(uint64_t now_us);
app_status_t esp_at_service_process(uint64_t now_us);
app_status_t esp_at_service_queue_telemetry(app_telemetry_snapshot_t const * p_snapshot);
app_status_t esp_at_service_close(void);
void esp_at_service_get_diagnostics(esp_at_service_diagnostics_t * p_diagnostics);
char const * esp_at_service_state_name(esp_at_service_state_t state);

#endif /* ESP_AT_SERVICE_H_ */

#include "app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal_data.h"
#include "fsp_version.h"
#include "ai_interaction.h"
#include "app_command.h"
#include "app_telemetry.h"
#include "camera_capture.h"
#include "camera_sensor.h"
#include "crc32_ieee.h"
#include "environment_service.h"
#include "esp_at_service.h"
#include "fault_record.h"
#include "lcd_display_service.h"
#include "lcd_i8080_bus.h"
#include "lcd_preview_scaler.h"
#include "lcd_result_overlay.h"
#include "mission_plan.h"
#include "platform_time.h"
#include "sccb_bus.h"
#include "sdram_test.h"
#include "sd_media.h"
#include "storage_service.h"
#include "uart_console.h"
#include "user_button.h"
#include "wheat_ai_backend.h"

#define APP_LED_PIN                    BSP_IO_PORT_01_PIN_10
#define APP_LED_PERIOD_US              (500000ULL)
#define APP_HEARTBEAT_PERIOD_US        (1000000ULL)
#define APP_CAMERA_SIGNAL_PROBE_US     (100000U)
#define APP_USER_BUTTON_PIN            BSP_IO_PORT_03_PIN_03
#define APP_SDRAM_BASE_ADDRESS         (0x68000000UL)
#define APP_SDRAM_BYTES                (16UL * 1024UL * 1024UL)
#define APP_AI_SNAPSHOT_BYTES          CAMERA_FRAME_BYTES
#define APP_AI_FREEZE_TIMEOUT_US       (500000ULL)
#define APP_AI_HEARTBEAT_DIVISOR       (4U)
#define APP_FIELD_TELEMETRY_PERIOD_US  (250000ULL)
#define APP_DHT11_START_DELAY_US        (1000000ULL)
#define APP_DHT11_PERIOD_US             (1000000ULL)
#define APP_LIGHT_START_DELAY_US         (100000ULL)
#define APP_LIGHT_PERIOD_US              (100000ULL)
#define APP_NETWORK_INIT_RETRY_US        (5000000ULL)
#define APP_NETWORK_HEARTBEAT_DIVISOR    (4U)
#define APP_STORAGE_HEARTBEAT_DIVISOR    (4U)
#define APP_FIELD_FAULT_SUBSYSTEM_APP  (1U)
#define APP_FIELD_FAULT_OPERATION_RUN  (1U)
#define APP_FIELD_FAULT_ACTIVE         (1UL << 0)
#define APP_FIELD_FAULT_HISTORY_LOST   (1UL << 1)

#if BSP_CFG_DCACHE_ENABLED
 #error "The camera/SDRAM/NPU ownership path must be cache-safe before enabling D-Cache."
#endif


typedef struct st_app_camera_signal_probe
{
    uint32_t samples;
    uint32_t pclk_high_samples;
    uint32_t pclk_transitions;
    uint32_t href_high_samples;
    uint32_t href_transitions;
    uint32_t vsync_high_samples;
    uint32_t vsync_transitions;
} app_camera_signal_probe_t;

static bool         g_initialized;
static bool         g_led_on;
static bool         g_camera_open;
static bool         g_lcd_available;
static bool         g_button_ready;
static bool         g_sdram_ready;
static bool         g_sdram_init_called;
static bool         g_hold_started;
static bool         g_ai_lease_active;
static bool         g_ai_snapshot_valid;
static bool         g_ai_backend_active;
static uint32_t     g_heartbeat_count;
static uint32_t     g_last_lcd_frame_count;
static uint64_t     g_led_deadline_us;
static uint64_t     g_heartbeat_deadline_us;
static uint64_t     g_last_heartbeat_us;
static uint64_t     g_ai_freeze_deadline_us;
static uint64_t     g_ai_transaction_start_us;
static app_status_t g_camera_status;
static app_status_t g_lcd_status;
static app_status_t g_sdram_status;
static app_status_t g_ai_status;
static app_status_t g_last_status;
static user_button_t g_user_button;
static ai_interaction_t g_ai_interaction;
static camera_frame_lease_t g_ai_lease;
static camera_frame_t g_ai_snapshot_frame;
static wheat_detection_result_t g_ai_result;
static uint8_t g_ai_snapshot_storage[APP_AI_SNAPSHOT_BYTES]
    __attribute__((aligned(32), section(".sdram_noinit")));
static uint32_t g_ai_held_crc32;
static uint32_t g_ai_preprocess_us;
static uint32_t g_ai_backend_us;
static uint32_t g_ai_postprocess_us;
static uint32_t g_ai_overlay_us;
static uint32_t g_ai_total_us;
static uint32_t g_ai_lease_acquires;
static uint32_t g_ai_lease_releases;
static uint32_t g_ai_lease_release_errors;
static uint32_t g_ai_snapshot_copy_us;
static uint32_t g_ai_snapshot_crc32;
static uint32_t g_ai_snapshot_copies;
static uint32_t g_ai_snapshot_errors;
static uint32_t g_mission_wheat_count;
static uint32_t g_field_wheat_count;
static uint32_t g_counted_frame_count;
static bool g_field_foundations_ready;
static bool g_environment_ready;
static bool g_network_ready;
static bool g_storage_ready;
static uint32_t g_field_fault_sequence;
static uint64_t g_field_last_now_us;
static uint64_t g_field_telemetry_deadline_us;
static uint64_t g_dht11_deadline_us;
static uint64_t g_light_deadline_us;
static uint64_t g_network_init_deadline_us;
static app_status_t g_environment_status;
static app_status_t g_network_status;
static environment_service_status_t g_environment_snapshot;
static esp_at_service_diagnostics_t g_network_snapshot;
static esp_at_service_state_t g_network_last_logged_state;
static uint32_t g_network_last_telemetry_sequence;
static uint32_t g_storage_last_telemetry_sequence;
static app_status_t g_storage_status;
static storage_service_diagnostics_t g_storage_snapshot;
static app_telemetry_store_t g_field_telemetry_store;
static app_telemetry_snapshot_t g_field_telemetry;
static app_command_cache_t g_field_command_cache;
static fault_record_queue_t g_field_fault_records;
static mission_plan_v1_t g_field_mission_plan;

void app_user_button_irq_callback(external_irq_callback_args_t * p_args);

static app_status_t app_status_from_uart(uart_console_status_t status);
static void         app_record_error(app_status_t status);
static void         app_service_led(uint64_t now_us);
static void         app_service_heartbeat(uint64_t now_us);
static app_status_t app_init_field_foundations(void);
static app_status_t app_service_field_foundations(uint64_t now_us);
static void         app_service_environment(uint64_t now_us);
static void         app_service_network(uint64_t now_us);
static void         app_service_storage(uint64_t now_us);
static void         app_copy_text(char * p_destination,
                                  size_t destination_bytes,
                                  char const * p_source);
static unsigned long app_log_u64_us(uint64_t value);
static char const * app_camera_state_name(camera_capture_state_t state);
static void         app_print_camera_summary(camera_sensor_test_result_t const * p_result);
static void         app_print_mode_summary(app_status_t status);
static void         app_print_ceu_summary(app_status_t status);
static void         app_print_camera_signal_probe(void);
static app_status_t app_init_user_button(uint64_t now_us);
static app_status_t app_init_sdram_controller(void);
static app_status_t app_validate_sdram(void);
static app_status_t app_service_user_button(uint64_t now_us);
static app_status_t app_service_ai_interaction(uint64_t now_us);
static app_status_t app_capture_ai_snapshot(void);
static app_status_t app_run_ai_transaction(uint64_t now_us);
static app_status_t app_resume_live_preview(void);
static app_status_t app_release_ai_lease(void);
static app_status_t app_cancel_pending_freeze(app_status_t status);
static void         app_fail_ai_transaction(app_status_t status);
#if BSP_CFG_SDRAM_ENABLED
static app_status_t app_sdram_progress(sdram_test_phase_t phase,
                                      uint32_t completed_words,
                                      void * p_context);
#endif

app_status_t app_init (void)
{
    app_status_t                bus_status;
    app_status_t                sensor_status;
    app_status_t                time_status;
    app_status_t                field_status;
    camera_sensor_test_result_t test_result = {0};
    lcd_i8080_bus_diagnostics_t lcd_bus_diagnostics = {0};
    camera_mode_t const         mode =
    {
        .width        = CAMERA_SENSOR_VGA_WIDTH,
        .height       = CAMERA_SENSOR_VGA_HEIGHT,
        .pixel_format = CAMERA_PIXEL_FORMAT_RGB565_BE,
        .frame_rate   = CAMERA_SENSOR_VGA_FRAME_RATE,
    };
    uint64_t                    now_us = 0U;
    fsp_err_t                   io_err;
    uart_console_status_t       uart_status;

    g_initialized           = true;
    g_led_on                = false;
    g_camera_open           = false;
    g_lcd_available         = false;
    g_button_ready          = false;
    g_sdram_ready           = false;
    g_sdram_init_called     = false;
    g_hold_started          = false;
    g_ai_lease_active       = false;
    g_ai_snapshot_valid     = false;
    g_ai_backend_active     = false;
    g_heartbeat_count       = 0U;
    g_last_lcd_frame_count  = 0U;
    g_led_deadline_us       = 0U;
    g_heartbeat_deadline_us = 0U;
    g_last_heartbeat_us     = 0U;
    g_ai_freeze_deadline_us = 0U;
    g_ai_transaction_start_us = 0U;
    g_camera_status         = APP_STATUS_NOT_INITIALIZED;
    g_lcd_status            = APP_STATUS_NOT_INITIALIZED;
    g_sdram_status          = APP_STATUS_NOT_INITIALIZED;
    g_ai_status             = APP_STATUS_NOT_INITIALIZED;
    g_last_status           = APP_STATUS_OK;
    g_user_button           = (user_button_t) {0};
    g_ai_lease              = (camera_frame_lease_t) {0};
    g_ai_snapshot_frame     = (camera_frame_t) {0};
    g_ai_result             = (wheat_detection_result_t) {0};
    g_mission_wheat_count   = 0U;
    g_field_wheat_count     = 0U;
    g_counted_frame_count   = 0U;
    g_ai_held_crc32         = 0U;
    g_ai_preprocess_us      = 0U;
    g_ai_backend_us         = 0U;
    g_ai_postprocess_us     = 0U;
    g_ai_overlay_us         = 0U;
    g_ai_total_us           = 0U;
    g_ai_lease_acquires     = 0U;
    g_ai_lease_releases     = 0U;
    g_ai_lease_release_errors = 0U;
    g_ai_snapshot_copy_us   = 0U;
    g_ai_snapshot_crc32     = 0U;
    g_ai_snapshot_copies    = 0U;
    g_ai_snapshot_errors    = 0U;
    g_field_foundations_ready = false;
    g_environment_ready = false;
    g_network_ready = false;
    g_storage_ready = false;
    g_field_fault_sequence = 0U;
    g_field_last_now_us = 0U;
    g_field_telemetry_deadline_us = 0U;
    g_dht11_deadline_us = 0U;
    g_light_deadline_us = 0U;
    g_network_init_deadline_us = 0U;
    g_environment_status = APP_STATUS_NOT_INITIALIZED;
    g_network_status = APP_STATUS_NOT_INITIALIZED;
    g_environment_snapshot = (environment_service_status_t) {0};
    g_network_snapshot = (esp_at_service_diagnostics_t) {0};
    g_network_last_logged_state = ESP_AT_SERVICE_STATE_CLOSED;
    g_network_last_telemetry_sequence = 0U;
    g_storage_last_telemetry_sequence = 0U;
    g_storage_status = APP_STATUS_NOT_INITIALIZED;
    g_storage_snapshot = (storage_service_diagnostics_t) {0};
    ai_interaction_init(&g_ai_interaction);

    field_status = app_init_field_foundations();
    if (APP_STATUS_OK != field_status)
    {
        app_record_error(field_status);
        return g_last_status;
    }

    io_err = g_ioport.p_api->pinWrite(g_ioport.p_ctrl, APP_LED_PIN, BSP_IO_LEVEL_LOW);
    if (FSP_SUCCESS != io_err)
    {
        app_record_error(APP_STATUS_IO);
    }

    uart_status = uart_console_init();
    if (UART_CONSOLE_STATUS_OK != uart_status)
    {
        app_record_error(app_status_from_uart(uart_status));
        return g_last_status;
    }

    (void) setvbuf(stdout, NULL, _IONBF, 0);
    if (printf("[startup] project=camera fsp=%s core=CPU0 uart=SCI9 115200 8N1 flow=none\r\n",
               FSP_VERSION_STRING) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }

    time_status = platform_time_init();
    if (APP_STATUS_OK == time_status)
    {
        time_status = platform_time_now(&now_us);
    }
    if (APP_STATUS_OK != time_status)
    {
        app_record_error(time_status);
        return g_last_status;
    }
    g_field_last_now_us = now_us;

    g_sdram_status = app_validate_sdram();
    if ((APP_STATUS_OK != g_sdram_status) && (APP_STATUS_UNAVAILABLE != g_sdram_status))
    {
        app_record_error(g_sdram_status);
    }

    g_ai_status = wheat_ai_backend_init();
    {
        wheat_ai_backend_info_t backend_info;

        wheat_ai_backend_get_info(&backend_info);
        if (printf("[ai] backend=%s model=%s model_hash=%s available=%s "
                   "model_src=0x%08lx model_dst=0x%08lx ospi_cfg_bytes=%lu ospi_jedec=%s src_crc=0x%08lx "
                   "dst_crc=0x%08lx cmd_src=0x%08lx cmd_dst=0x%08lx cmd_src_crc=0x%08lx "
                   "cmd_dst_crc=0x%08lx sanity_source_crc=0x%08lx sanity_board_crc=0x%08lx "
                   "sanity_actual_crc=0x%08lx numeric_gate=%s requests=%lu "
                   "npu_invocations=%lu init_status=%u\r\n",
                   backend_info.name,
                   backend_info.model_identity,
                   backend_info.model_hash,
                   backend_info.model_available ? "yes" : "no",
                   (unsigned long) backend_info.storage_source_address,
                   (unsigned long) backend_info.storage_runtime_address,
                   (unsigned long) backend_info.storage_configured_capacity_bytes,
                   backend_info.storage_jedec_identity_verified ? "verified" : "pending",
                   (unsigned long) backend_info.storage_source_crc32,
                   (unsigned long) backend_info.storage_runtime_crc32,
                   (unsigned long) backend_info.command_stream_source_address,
                   (unsigned long) backend_info.command_stream_runtime_address,
                   (unsigned long) backend_info.command_stream_source_crc32,
                   (unsigned long) backend_info.command_stream_runtime_crc32,
                   (unsigned long) backend_info.source_sanity_output_crc32,
                   (unsigned long) backend_info.qualified_board_sanity_output_crc32,
                   (unsigned long) backend_info.deterministic_sanity_output_crc32,
                   backend_info.board_numeric_gate_passed ? "pass" : "pending",
                   (unsigned long) backend_info.requests,
                   (unsigned long) backend_info.hardware_invocations,
                   (unsigned int) g_ai_status) < 0)
        {
            app_record_error(APP_STATUS_IO);
        }
    }

    bus_status = sccb_bus_init();
    sensor_status = camera_sensor_open();
    g_camera_status = (APP_STATUS_OK != bus_status) ? bus_status : sensor_status;

    if (printf("[camera] bus=%s rate=%lu address=0x%02x\r\n",
               SCCB_BUS_INSTANCE_NAME,
               (unsigned long) SCCB_BUS_RATE_HZ,
               (unsigned int) SCCB_BUS_SLAVE_ADDRESS) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }

    if (APP_STATUS_OK == g_camera_status)
    {
        g_camera_status = camera_sensor_id_self_test(CAMERA_SENSOR_ID_TEST_CYCLES, &test_result);
    }
    else
    {
        test_result.cycles_requested = CAMERA_SENSOR_ID_TEST_CYCLES;
        test_result.status = g_camera_status;
    }
    app_print_camera_summary(&test_result);

    if (APP_STATUS_OK == g_camera_status)
    {
        g_camera_status = camera_sensor_apply_mode(&mode);
        if (APP_STATUS_OK == g_camera_status)
        {
            g_camera_status = camera_sensor_start();
        }
        app_print_mode_summary(g_camera_status);
    }

    if (APP_STATUS_OK == g_camera_status)
    {
        g_camera_status = camera_capture_open();
        g_camera_open = (APP_STATUS_OK == g_camera_status);
        app_print_ceu_summary(g_camera_status);
    }

    /* Blocking SDRAM/model/camera bring-up is a separate timing epoch.  Start
     * the cooperative runtime clock only after those bounded init steps, so
     * the 2 s main-loop service-gap monitor does not reject expected startup. */
    time_status = platform_time_begin_runtime_epoch();
    if (APP_STATUS_OK == time_status)
    {
        time_status = platform_time_now(&now_us);
    }
    if (APP_STATUS_OK != time_status)
    {
        app_record_error(time_status);
        return g_last_status;
    }
    g_field_last_now_us = now_us;

    time_status = app_init_user_button(now_us);
    if (APP_STATUS_OK != time_status)
    {
        app_record_error(time_status);
    }
    if (printf("[time] epoch=runtime max_service_gap_us=%lu status=ok\r\n",
               (unsigned long) PLATFORM_TIME_MAX_SERVICE_US) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }

    g_environment_status = environment_service_init(now_us);
    (void) environment_service_get_status(now_us, &g_environment_snapshot);
    if (APP_STATUS_OK == g_environment_status)
    {
        g_environment_ready = true;
        g_field_telemetry.capabilities |= RA8P_CAPABILITY_ENVIRONMENT;
        g_dht11_deadline_us = now_us + APP_DHT11_START_DELAY_US;
        g_light_deadline_us = now_us + APP_LIGHT_START_DELAY_US;
    }
    if (printf("[environment] dht11=P010/IRQ14 status=%s code=%u period_ms=%lu "
               "light=P513/ADC0_AN016 status=%s code=%u period_ms=%lu service=%s\r\n",
               g_environment_snapshot.dht11_available ? "ready" : "offline",
               (unsigned int) g_environment_snapshot.hardware.dht11_init_status,
               (unsigned long) (APP_DHT11_PERIOD_US / 1000ULL),
               g_environment_snapshot.light_available ? "ready" :
               ((APP_STATUS_BUSY ==
                 g_environment_snapshot.hardware.light_init_status) ?
                "calibrating" : "offline"),
               (unsigned int) g_environment_snapshot.hardware.light_init_status,
               (unsigned long) (APP_LIGHT_PERIOD_US / 1000ULL),
               g_environment_ready ? "ready" : "degraded") < 0)
    {
        app_record_error(APP_STATUS_IO);
    }

    g_network_status = esp_at_service_init(now_us);
    g_network_ready = (APP_STATUS_OK == g_network_status);
    if (g_network_ready)
    {
        g_field_telemetry.capabilities |= RA8P_CAPABILITY_LAN_TCP;
        esp_at_service_get_diagnostics(&g_network_snapshot);
        g_network_last_logged_state = g_network_snapshot.state;
    }
    else
    {
        g_network_init_deadline_us = now_us + APP_NETWORK_INIT_RETRY_US;
    }
    if (printf("[network] esp8266=SCI0 pins=P602/P603 baud=115200 flow=none "
               "tcp_server=%u credentials=%s state=%s service=%s code=%u\r\n",
               (unsigned int) ESP_AT_SERVICE_TCP_PORT,
               g_network_snapshot.credentials_configured ? "local" : "missing",
               esp_at_service_state_name(g_network_snapshot.state),
               g_network_ready ? "ready" : "degraded",
               (unsigned int) g_network_status) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }

    g_storage_status = storage_service_init(now_us);
    g_storage_ready = (APP_STATUS_OK == g_storage_status);
    storage_service_get_diagnostics(&g_storage_snapshot);
    if (printf("[storage] media=SDHI0 bus=4bit dmac=DMAC0 fatfs=R0.15 "
               "queue=8 binary=FIELD.BIN csv=FIELD.CSV state=%u service=%s code=%u\r\n",
               (unsigned int) g_storage_snapshot.state,
               g_storage_ready ? "ready" : "degraded",
               (unsigned int) g_storage_status) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }

    g_lcd_status = lcd_display_service_init(now_us);
    g_lcd_available = (APP_STATUS_OK == g_lcd_status);
    lcd_i8080_bus_get_diagnostics(&lcd_bus_diagnostics);
    if (printf("[lcd] controller=ST7796S panel=480x320 bus=I8080-16 backend=grouped-gpio "
                "preview=%ux%u origin=%u,%u bars=%u/%u scaler=%s "
                "slice_rows=%u wr_nops=%u spec_min_ns=%u/%u/%u "
                "cache=off dmac=deferred status=%s code=%u\r\n",
               (unsigned int) LCD_PREVIEW_IMAGE_WIDTH,
               (unsigned int) LCD_PREVIEW_IMAGE_HEIGHT,
               (unsigned int) LCD_PREVIEW_X_OFFSET,
               (unsigned int) LCD_PREVIEW_Y_OFFSET,
               (unsigned int) LCD_PREVIEW_SIDE_BAR_WIDTH,
               (unsigned int) LCD_PREVIEW_SIDE_BAR_WIDTH,
                LCD_PREVIEW_SCALER_NAME,
                (unsigned int) LCD_PREVIEW_ROWS_PER_SLICE,
                (unsigned int) lcd_bus_diagnostics.strobe_delay_iterations,
                (unsigned int) lcd_bus_diagnostics.controller_write_cycle_min_ns,
                (unsigned int) lcd_bus_diagnostics.controller_control_pulse_min_ns,
                (unsigned int) lcd_bus_diagnostics.controller_data_setup_hold_min_ns,
                g_lcd_available ? "ok" : "error",
               (unsigned int) g_lcd_status) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }

    if (APP_STATUS_OK == g_camera_status)
    {
        app_print_camera_signal_probe();
        g_camera_status = camera_capture_stream_start();
    }
    if (APP_STATUS_OK == g_camera_status)
    {
        if (printf("[camera] stream mode=ping-pong buffers=2 policy=latest consumer=lcd status=ok\r\n") < 0)
        {
            app_record_error(APP_STATUS_IO);
        }
    }
    else
    {
        app_record_error(g_camera_status);
    }
    if (!g_lcd_available)
    {
        app_record_error(g_lcd_status);
    }

    time_status = platform_time_now(&now_us);
    if (APP_STATUS_OK != time_status)
    {
        app_record_error(time_status);
    }
    else
    {
        g_field_last_now_us = now_us;
    }
    g_led_deadline_us       = now_us + APP_LED_PERIOD_US;
    g_heartbeat_deadline_us = now_us + APP_HEARTBEAT_PERIOD_US;
    g_last_heartbeat_us     = now_us;

    return g_last_status;
}

app_status_t app_poll (void)
{
    uint64_t     now_us = 0U;
    app_status_t status;

    if (!g_initialized)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }

    status = platform_time_now(&now_us);
    if (APP_STATUS_OK != status)
    {
        app_record_error(status);
        if (APP_STATUS_TIMEOUT != status)
        {
            return g_last_status;
        }
    }
    g_field_last_now_us = now_us;

    status = app_service_user_button(now_us);
    if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
    {
        app_record_error(status);
    }

    if (g_camera_open)
    {
        status = camera_capture_process(now_us);
        if (APP_STATUS_OK != status)
        {
            g_camera_status = status;
            app_record_error(status);
            app_fail_ai_transaction(status);

            if (g_lcd_available)
            {
                g_lcd_status = lcd_display_service_close();
                g_lcd_available = false;
                app_record_error(g_lcd_status);
            }
        }
    }

    {
        uart_console_status_t const uart_status = uart_console_process(now_us);

        if ((UART_CONSOLE_STATUS_OK != uart_status) &&
            (UART_CONSOLE_STATUS_BUSY != uart_status))
        {
            app_record_error(app_status_from_uart(uart_status));
        }
    }

    status = app_service_ai_interaction(now_us);
    if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status) &&
        (APP_STATUS_UNAVAILABLE != status))
    {
        app_record_error(status);
    }

    if (g_lcd_available && (APP_STATUS_OK == g_camera_status))
    {
        status = lcd_display_service_process(now_us);
        if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
        {
            g_lcd_status = status;
            g_lcd_available = false;
            app_record_error(status);
            app_fail_ai_transaction(status);
        }
    }

    app_service_environment(now_us);

    status = app_service_field_foundations(now_us);
    if (APP_STATUS_OK != status)
    {
        app_record_error(status);
    }

    app_service_network(now_us);
    app_service_storage(now_us);

    app_service_led(now_us);
    app_service_heartbeat(now_us);

    return g_last_status;
}

static app_status_t app_init_user_button (uint64_t now_us)
{
    bsp_io_level_t level = BSP_IO_LEVEL_HIGH;
    fsp_err_t err = g_ioport.p_api->pinRead(g_ioport.p_ctrl,
                                            APP_USER_BUTTON_PIN,
                                            &level);

    if (FSP_SUCCESS != err)
    {
        if (printf("[key] pin=P303 active=low pullup=internal status=error fsp=%ld\r\n",
                   (long) err) < 0)
        {
            app_record_error(APP_STATUS_IO);
        }
        return APP_STATUS_IO;
    }

    user_button_init(&g_user_button, BSP_IO_LEVEL_LOW == level, now_us);
    err = g_user_button_irq.p_api->open(g_user_button_irq.p_ctrl,
                                        g_user_button_irq.p_cfg);
    if (FSP_SUCCESS != err)
    {
        if (printf("[key] pin=P303 irq=IRQ29-DS step=open status=error fsp=%ld\r\n",
                   (long) err) < 0)
        {
            app_record_error(APP_STATUS_IO);
        }
        return APP_STATUS_IO;
    }

    err = g_user_button_irq.p_api->enable(g_user_button_irq.p_ctrl);
    if (FSP_SUCCESS != err)
    {
        (void) g_user_button_irq.p_api->close(g_user_button_irq.p_ctrl);
        if (printf("[key] pin=P303 irq=IRQ29-DS step=enable status=error fsp=%ld\r\n",
                   (long) err) < 0)
        {
            app_record_error(APP_STATUS_IO);
        }
        return APP_STATUS_IO;
    }

    g_button_ready = true;
    if (printf("[key] pin=P303 active=low pullup=internal source=IRQ29-DS "
               "trigger=falling filter=pclk/64 release_debounce_ms=30 initial=%s status=ok\r\n",
               (BSP_IO_LEVEL_LOW == level) ? "pressed" : "released") < 0)
    {
        return APP_STATUS_IO;
    }

    return APP_STATUS_OK;
}

static app_status_t app_validate_sdram (void)
{
#if BSP_CFG_SDRAM_ENABLED
    sdram_test_result_t result;
    uint64_t start_us = 0U;
    uint64_t end_us = 0U;
    app_status_t status = app_init_sdram_controller();

    if (APP_STATUS_OK != status)
    {
        return status;
    }
    status = platform_time_now(&start_us);
    if (APP_STATUS_OK != status)
    {
        return status;
    }
    if (printf("[sdram] base=0x%08lx bytes=%lu bus=32 cache=off test=start\r\n",
               (unsigned long) APP_SDRAM_BASE_ADDRESS,
               (unsigned long) APP_SDRAM_BYTES) < 0)
    {
        return APP_STATUS_IO;
    }

    status = sdram_test_run((volatile uint32_t *) APP_SDRAM_BASE_ADDRESS,
                            APP_SDRAM_BYTES,
                            app_sdram_progress,
                            NULL,
                            &result);
    {
        app_status_t const time_status = platform_time_now(&end_us);

        if ((APP_STATUS_OK == status) && (APP_STATUS_OK != time_status))
        {
            status = time_status;
        }
    }

    g_sdram_ready = (APP_STATUS_OK == status) && result.passed;
    if (printf("[sdram] status=%s code=%u phase=%s address=0x%08lx word=%lu "
               "expected=0x%08lx actual=0x%08lx verified=%lu elapsed_us=%lu\r\n",
               g_sdram_ready ? "pass" : "fail",
               (unsigned int) status,
               sdram_test_phase_name(result.phase),
               (unsigned long) result.first_failure_address,
               (unsigned long) result.first_failure_word,
               (unsigned long) result.expected,
               (unsigned long) result.actual,
               (unsigned long) result.words_verified,
               app_log_u64_us(end_us - start_us)) < 0)
    {
        return APP_STATUS_IO;
    }

    return g_sdram_ready ? APP_STATUS_OK : status;
#else
    g_sdram_ready = false;
    if (printf("[sdram] status=disabled action=generate-project-content code=%u\r\n",
               (unsigned int) APP_STATUS_UNAVAILABLE) < 0)
    {
        return APP_STATUS_IO;
    }
    return APP_STATUS_UNAVAILABLE;
#endif
}

static app_status_t app_init_sdram_controller (void)
{
#if BSP_CFG_SDRAM_ENABLED
    bool const already_enabled = (0U != R_BUS->SDRAM.SDCCR_b.EXENB);

    if (!already_enabled)
    {
        if (g_sdram_init_called)
        {
            if (printf("[sdram-init] source=app action=reject-repeat exenb=0 status=%u\r\n",
                       (unsigned int) APP_STATUS_DATA) < 0)
            {
                return APP_STATUS_IO;
            }
            return APP_STATUS_DATA;
        }

        g_sdram_init_called = true;
        R_BSP_SdramInit(true);
    }

    if (0U == R_BUS->SDRAM.SDCCR_b.EXENB)
    {
        if (printf("[sdram-init] source=%s action=verify exenb=0 status=%u\r\n",
                   already_enabled ? "startup" : "app",
                   (unsigned int) APP_STATUS_DATA) < 0)
        {
            return APP_STATUS_IO;
        }
        return APP_STATUS_DATA;
    }

    if (printf("[sdram-init] source=%s action=%s exenb=1 status=ok\r\n",
               already_enabled ? "startup" : "app",
               already_enabled ? "reuse" : "initialize") < 0)
    {
        return APP_STATUS_IO;
    }
    return APP_STATUS_OK;
#else
    return APP_STATUS_UNAVAILABLE;
#endif
}

static app_status_t app_service_user_button (uint64_t now_us)
{
    bsp_io_level_t level = BSP_IO_LEVEL_HIGH;
    user_button_event_t event;
    user_button_diagnostics_t button_diagnostics;
    ai_interaction_action_t action;
    fsp_err_t err;
    app_status_t status = APP_STATUS_OK;

    if (!g_button_ready)
    {
        return APP_STATUS_OK;
    }

    err = g_ioport.p_api->pinRead(g_ioport.p_ctrl, APP_USER_BUTTON_PIN, &level);
    if (FSP_SUCCESS != err)
    {
        return APP_STATUS_IO;
    }

    (void) user_button_update(&g_user_button, BSP_IO_LEVEL_LOW == level, now_us);
    event = user_button_consume_press(&g_user_button);
    if (USER_BUTTON_EVENT_PRESSED != event)
    {
        return APP_STATUS_OK;
    }

    action = ai_interaction_on_press(&g_ai_interaction);
    if (AI_INTERACTION_ACTION_REQUEST_FREEZE == action)
    {
        if (!g_camera_open || !g_lcd_available)
        {
            status = APP_STATUS_UNAVAILABLE;
            (void) ai_interaction_cancel_freeze(&g_ai_interaction, status);
        }
        else
        {
            g_hold_started = false;
            g_ai_freeze_deadline_us = now_us + APP_AI_FREEZE_TIMEOUT_US;
            /* The retained frame lease protects the inference image while CEU
             * continues capturing into the other pool slot. */
            status = APP_STATUS_OK;
        }
    }
    else if (AI_INTERACTION_ACTION_RESUME_PREVIEW == action)
    {
        status = app_resume_live_preview();
    }
    else if (AI_INTERACTION_ACTION_REPORT_BUSY == action)
    {
        user_button_record_ignored_busy(&g_user_button);
        status = APP_STATUS_BUSY;
    }

    user_button_get_diagnostics(&g_user_button, &button_diagnostics);
    if (printf("[key] event=press n=%lu irq=%lu consumed=%lu pending=%lu "
               "ignored_irq=%lu ignored_busy=%lu action=%u state=%s status=%u\r\n",
               (unsigned long) button_diagnostics.press_events,
               (unsigned long) button_diagnostics.irq_events,
               (unsigned long) button_diagnostics.consumed_events,
               (unsigned long) button_diagnostics.pending_press_events,
               (unsigned long) button_diagnostics.ignored_irq_events,
               (unsigned long) button_diagnostics.ignored_busy_events,
               (unsigned int) action,
               ai_interaction_state_name(g_ai_interaction.state),
               (unsigned int) status) < 0)
    {
        return APP_STATUS_IO;
    }

    return status;
}

void app_user_button_irq_callback (external_irq_callback_args_t * p_args)
{
    if ((NULL != p_args) && (29U == p_args->channel))
    {
        user_button_latch_press_from_isr(&g_user_button);
    }
}

static app_status_t app_service_ai_interaction (uint64_t now_us)
{
    if (AI_INTERACTION_FREEZE_PENDING == g_ai_interaction.state)
    {
        camera_capture_diagnostics_t camera_diagnostics;

        camera_capture_get_diagnostics(&camera_diagnostics);
        if (CAMERA_CAPTURE_STATE_STREAMING != camera_diagnostics.state)
        {
            app_status_t const status = (CAMERA_CAPTURE_STATE_ERROR == camera_diagnostics.state) ?
                                        camera_diagnostics.last_status : APP_STATUS_BUSY;

            if (APP_STATUS_BUSY != status)
            {
                app_fail_ai_transaction(status);
            }
            return status;
        }

        if (!g_hold_started)
        {
            app_status_t status;

            if ((0U != g_ai_freeze_deadline_us) &&
                (now_us >= g_ai_freeze_deadline_us))
            {
                return app_cancel_pending_freeze(APP_STATUS_TIMEOUT);
            }

            if (!lcd_display_service_ready_for_hold())
            {
                return APP_STATUS_BUSY;
            }

            if (!g_ai_snapshot_valid)
            {
                status = app_capture_ai_snapshot();
                if (APP_STATUS_BUSY == status)
                {
                    return APP_STATUS_BUSY;
                }
                if (APP_STATUS_OK != status)
                {
                    return app_cancel_pending_freeze(status);
                }
            }

            status = lcd_display_service_begin_frame_hold(&g_ai_snapshot_frame);
            if (APP_STATUS_BUSY == status)
            {
                return APP_STATUS_BUSY;
            }
            if (APP_STATUS_OK != status)
            {
                return app_cancel_pending_freeze(status);
            }
            g_hold_started = true;
            g_ai_freeze_deadline_us = 0U;
            if (printf("[ai-frame] state=freeze storage=sdram address=0x%08lx "
                       "sequence=%lu generation=%lu crc=0x%08lx copy_us=%lu "
                       "status=rendering\r\n",
                       (unsigned long) (uintptr_t) g_ai_snapshot_storage,
                       (unsigned long) g_ai_snapshot_frame.sequence,
                       (unsigned long) g_ai_snapshot_frame.generation,
                       (unsigned long) g_ai_snapshot_frame.crc32,
                       (unsigned long) g_ai_snapshot_copy_us) < 0)
            {
                return APP_STATUS_IO;
            }
        }

        if (lcd_display_service_hold_complete())
        {
            if (AI_INTERACTION_ACTION_RUN_INFERENCE ==
                ai_interaction_on_frame_held(&g_ai_interaction))
            {
                return app_run_ai_transaction(now_us);
            }
        }

        return APP_STATUS_BUSY;
    }

    if (AI_INTERACTION_INFERENCING == g_ai_interaction.state)
    {
        return app_run_ai_transaction(now_us);
    }

    return APP_STATUS_OK;
}

static app_status_t app_capture_ai_snapshot (void)
{
    uint32_t const copy_start_cycles = platform_time_raw_cycles();
    app_status_t status;
    app_status_t release_status;

    if (!g_sdram_ready || g_ai_snapshot_valid || g_ai_lease_active)
    {
        return g_ai_snapshot_valid ? APP_STATUS_BUSY : g_sdram_status;
    }

    status = camera_capture_acquire_latest(&g_ai_lease);
    if (APP_STATUS_OK != status)
    {
        return status;
    }
    g_ai_lease_active = true;
    g_ai_lease_acquires++;

    if ((NULL == g_ai_lease.p_frame) ||
        (CAMERA_FRAME_BYTES != g_ai_lease.p_frame->valid_bytes) ||
        (CAMERA_FRAME_WIDTH != g_ai_lease.p_frame->width) ||
        (CAMERA_FRAME_HEIGHT != g_ai_lease.p_frame->height) ||
        (CAMERA_FRAME_STRIDE != g_ai_lease.p_frame->stride) ||
        (CAMERA_PIXEL_FORMAT_RGB565_BE != g_ai_lease.p_frame->pixel_format))
    {
        status = APP_STATUS_DATA;
    }

    if (APP_STATUS_OK == status)
    {
        (void) memcpy(g_ai_snapshot_storage,
                      (void const *) g_ai_lease.p_frame->address,
                      APP_AI_SNAPSHOT_BYTES);
        status = crc32_ieee_calculate(g_ai_snapshot_storage,
                                      APP_AI_SNAPSHOT_BYTES,
                                      &g_ai_snapshot_crc32);
    }
    if ((APP_STATUS_OK == status) &&
        (g_ai_snapshot_crc32 != g_ai_lease.p_frame->crc32))
    {
        status = APP_STATUS_DATA;
    }
    if (APP_STATUS_OK == status)
    {
        g_ai_snapshot_frame = *g_ai_lease.p_frame;
        g_ai_snapshot_frame.address = (uintptr_t) g_ai_snapshot_storage;
        g_ai_snapshot_frame.capacity = APP_AI_SNAPSHOT_BYTES;
        g_ai_snapshot_frame.valid_bytes = APP_AI_SNAPSHOT_BYTES;
        g_ai_snapshot_frame.crc32 = g_ai_snapshot_crc32;
    }

    release_status = app_release_ai_lease();
    if ((APP_STATUS_OK == status) && (APP_STATUS_OK != release_status))
    {
        status = release_status;
    }
    if (APP_STATUS_OK != status)
    {
        g_ai_snapshot_frame = (camera_frame_t) {0};
        g_ai_snapshot_valid = false;
        return status;
    }

    (void) platform_time_elapsed_us(copy_start_cycles,
                                    platform_time_raw_cycles(),
                                    &g_ai_snapshot_copy_us);
    g_ai_snapshot_valid = true;
    g_ai_snapshot_copies++;
    return APP_STATUS_OK;
}

static app_status_t app_run_ai_transaction (uint64_t now_us)
{
    uint32_t stage_start_cycles;
    uint64_t end_us = now_us;
    app_status_t status = APP_STATUS_OK;
    app_status_t backend_status = APP_STATUS_NOT_INITIALIZED;
    app_status_t overlay_status;
    app_status_t time_status;
    wheat_ai_backend_info_t backend_info;

    if ((AI_INTERACTION_INFERENCING != g_ai_interaction.state) ||
        !g_ai_snapshot_valid)
    {
        return APP_STATUS_DATA;
    }

    if (!g_ai_backend_active)
    {
        g_ai_result = (wheat_detection_result_t) {0};
        g_ai_held_crc32 = 0U;
        g_ai_preprocess_us = 0U;
        g_ai_backend_us = 0U;
        g_ai_postprocess_us = 0U;
        g_ai_overlay_us = 0U;
        g_ai_total_us = 0U;
        g_ai_transaction_start_us = now_us;
        status = crc32_ieee_calculate((uint8_t const *) g_ai_snapshot_frame.address,
                                      g_ai_snapshot_frame.valid_bytes,
                                      &g_ai_held_crc32);
        if ((APP_STATUS_OK == status) &&
            (g_ai_held_crc32 != g_ai_snapshot_frame.crc32))
        {
            status = APP_STATUS_DATA;
        }
        if ((APP_STATUS_OK == status) && !g_sdram_ready)
        {
            status = g_sdram_status;
        }
        if (APP_STATUS_OK == status)
        {
            backend_status = wheat_ai_backend_start_frame(
                (uint8_t const *) g_ai_snapshot_frame.address,
                g_ai_snapshot_frame.valid_bytes,
                g_ai_snapshot_frame.width,
                g_ai_snapshot_frame.height,
                g_ai_snapshot_frame.stride);
            status = backend_status;
            g_ai_backend_active = (APP_STATUS_OK == backend_status);
        }
    }

    if (g_ai_backend_active)
    {
        backend_status = wheat_ai_backend_process_frame_step(&g_ai_result);
        status = backend_status;
    }
    wheat_ai_backend_get_info(&backend_info);
    g_ai_preprocess_us = backend_info.last_preprocess_us;
    g_ai_backend_us = backend_info.last_inference_us;
    g_ai_postprocess_us = backend_info.last_postprocess_us;
    if (APP_STATUS_BUSY == status)
    {
        return APP_STATUS_BUSY;
    }
    g_ai_backend_active = false;

    stage_start_cycles = platform_time_raw_cycles();
    overlay_status = lcd_result_overlay_draw(&g_ai_result, status);
    (void) platform_time_elapsed_us(stage_start_cycles,
                                    platform_time_raw_cycles(),
                                    &g_ai_overlay_us);
    g_ai_status = (APP_STATUS_OK == overlay_status) ? status : overlay_status;
    if (AI_INTERACTION_ACTION_PRESENT_RESULT !=
         ai_interaction_on_inference_complete(&g_ai_interaction, g_ai_status))
    {
        return APP_STATUS_DATA;
    }

    time_status = platform_time_now(&end_us);
    if ((APP_STATUS_OK != time_status) && (APP_STATUS_TIMEOUT != time_status))
    {
        app_record_error(time_status);
    }
    if (end_us >= g_ai_transaction_start_us)
    {
        g_ai_total_us = (uint32_t) (((end_us - g_ai_transaction_start_us) > UINT32_MAX) ?
                                    UINT32_MAX :
                                    (end_us - g_ai_transaction_start_us));
    }

    if (APP_STATUS_OK == g_ai_status)
    {
        if (g_mission_wheat_count > (UINT32_MAX - g_ai_result.count))
        {
            g_mission_wheat_count = UINT32_MAX;
        }
        else
        {
            g_mission_wheat_count += g_ai_result.count;
        }
        if (g_field_wheat_count > (UINT32_MAX - g_ai_result.count))
        {
            g_field_wheat_count = UINT32_MAX;
        }
        else
        {
            g_field_wheat_count += g_ai_result.count;
        }
        if (g_counted_frame_count < UINT32_MAX)
        {
            g_counted_frame_count++;
        }
    }

    if (printf("[ai-cycle] state=%s frame=%lu held_crc=0x%08lx source_crc=0x%08lx "
               "tiles=%lu/12 tile=224x224 stride=179 edge_x=416 edge_y=256 "
               "layout=NHWC type=INT8 order=BGR scale=1 zp=-128 bytes=150528 "
               "preprocess_us=%lu inference_us=%lu postprocess_us=%lu "
               "overlay_us=%lu total_us=%lu backend_status=%u result_valid=%s "
               "scanned=%lu above=%lu pool_drop=%lu result_drop=%lu retained=%u "
               "overlay_status=%u cycle_status=%u\r\n",
               ai_interaction_state_name(g_ai_interaction.state),
               (unsigned long) g_ai_snapshot_frame.sequence,
               (unsigned long) g_ai_held_crc32,
               (unsigned long) g_ai_snapshot_frame.crc32,
               (unsigned long) backend_info.tiles_completed,
               (unsigned long) g_ai_preprocess_us,
               (unsigned long) g_ai_backend_us,
               (unsigned long) g_ai_postprocess_us,
               (unsigned long) g_ai_overlay_us,
               (unsigned long) g_ai_total_us,
               (unsigned int) backend_status,
               (APP_STATUS_OK == backend_status) ? "yes" : "no",
               (unsigned long) g_ai_result.candidates_scanned,
               (unsigned long) g_ai_result.candidates_above_threshold,
               (unsigned long) g_ai_result.candidates_dropped,
               (unsigned long) g_ai_result.detections_dropped,
               (unsigned int) g_ai_result.count,
               (unsigned int) overlay_status,
               (unsigned int) g_ai_status) < 0)
    {
        return APP_STATUS_IO;
    }

    return g_ai_status;
}

static app_status_t app_resume_live_preview (void)
{
    camera_capture_diagnostics_t camera_diagnostics;
    app_status_t status = APP_STATUS_OK;

    if (g_ai_backend_active)
    {
        wheat_ai_backend_cancel_frame(APP_STATUS_UNAVAILABLE);
        g_ai_backend_active = false;
    }

    if (g_ai_lease_active)
    {
        status = app_release_ai_lease();
        if (APP_STATUS_OK != status)
        {
            return status;
        }
    }

    if (!g_camera_open)
    {
        return APP_STATUS_UNAVAILABLE;
    }

    camera_capture_get_diagnostics(&camera_diagnostics);
    if (CAMERA_CAPTURE_STATE_STREAMING != camera_diagnostics.state)
    {
        status = (CAMERA_CAPTURE_STATE_ERROR == camera_diagnostics.state) ?
                 camera_diagnostics.last_status : APP_STATUS_UNAVAILABLE;
        g_camera_status = status;
        return status;
    }
    g_camera_status = APP_STATUS_OK;

    if ((APP_STATUS_OK == status) && g_lcd_available)
    {
        status = lcd_display_service_resume_live();
    }

    if (APP_STATUS_OK == status)
    {
        g_ai_snapshot_valid = false;
        g_ai_snapshot_frame = (camera_frame_t) {0};
        g_hold_started = false;
        g_ai_freeze_deadline_us = 0U;
        g_ai_transaction_start_us = 0U;
        (void) ai_interaction_on_resume_complete(&g_ai_interaction);
        g_ai_status = APP_STATUS_UNAVAILABLE;
        if (printf("[ai-frame] state=live-preview action=resume releases=%lu "
                   "release_errors=%lu status=ok\r\n",
                   (unsigned long) g_ai_lease_releases,
                   (unsigned long) g_ai_lease_release_errors) < 0)
        {
            return APP_STATUS_IO;
        }
    }

    return status;
}

static app_status_t app_release_ai_lease (void)
{
    app_status_t status;

    if (!g_ai_lease_active)
    {
        return APP_STATUS_OK;
    }

    status = camera_capture_release(&g_ai_lease);
    if (APP_STATUS_OK != status)
    {
        g_ai_lease_release_errors++;
        return status;
    }

    g_ai_lease_releases++;
    g_ai_lease_active = false;
    g_ai_lease = (camera_frame_lease_t) {0};
    return APP_STATUS_OK;
}

static app_status_t app_cancel_pending_freeze (app_status_t status)
{
    app_status_t const release_status = app_release_ai_lease();
    app_status_t display_status = APP_STATUS_OK;

    if (g_ai_backend_active)
    {
        wheat_ai_backend_cancel_frame(status);
        g_ai_backend_active = false;
    }

    if (g_hold_started && g_lcd_available)
    {
        display_status = lcd_display_service_resume_live();
    }

    g_ai_snapshot_valid = false;
    g_ai_snapshot_frame = (camera_frame_t) {0};
    g_hold_started = false;
    g_ai_freeze_deadline_us = 0U;
    g_ai_snapshot_errors++;
    g_ai_status = status;
    (void) ai_interaction_cancel_freeze(&g_ai_interaction, status);

    if (APP_STATUS_OK != release_status)
    {
        return release_status;
    }
    return (APP_STATUS_OK == display_status) ? status : display_status;
}

static void app_fail_ai_transaction (app_status_t status)
{
    if (AI_INTERACTION_FREEZE_PENDING == g_ai_interaction.state)
    {
        (void) app_cancel_pending_freeze(status);
    }
    else if (AI_INTERACTION_INFERENCING == g_ai_interaction.state)
    {
        wheat_ai_backend_cancel_frame(status);
        g_ai_backend_active = false;
        g_ai_status = status;
        (void) ai_interaction_on_transaction_failed(&g_ai_interaction, status);
    }
}

#if BSP_CFG_SDRAM_ENABLED
static app_status_t app_sdram_progress (sdram_test_phase_t phase,
                                        uint32_t completed_words,
                                        void * p_context)
{
    uint64_t now_us;

    (void) phase;
    (void) completed_words;
    (void) p_context;
    return platform_time_now(&now_us);
}
#endif

static app_status_t app_status_from_uart (uart_console_status_t status)
{
    return (UART_CONSOLE_STATUS_TIMEOUT == status) ? APP_STATUS_TIMEOUT : APP_STATUS_IO;
}

static void app_record_error (app_status_t status)
{
    if (APP_STATUS_OK != status)
    {
        fault_record_t record;

        g_last_status = status;
        if (g_field_foundations_ready)
        {
            g_field_fault_sequence++;
            record = (fault_record_t)
            {
                .sequence     = g_field_fault_sequence,
                .monotonic_us = g_field_last_now_us,
                .subsystem    = APP_FIELD_FAULT_SUBSYSTEM_APP,
                .operation    = APP_FIELD_FAULT_OPERATION_RUN,
                .status       = status,
                .detail       = (int32_t) status,
            };
            (void) fault_record_push(&g_field_fault_records, &record);
        }
    }
}

static app_status_t app_init_field_foundations (void)
{
    mission_validation_t validation;
    app_status_t status;

    g_field_telemetry = (app_telemetry_snapshot_t) {0};
    g_field_telemetry.schema_version = APP_TELEMETRY_SCHEMA_VERSION;
    g_field_telemetry.capabilities = APP_TELEMETRY_FOUNDATION_CAPABILITIES;
    g_field_telemetry.wheat_count = APP_TELEMETRY_WHEAT_COUNT_UNKNOWN;
    app_copy_text(g_field_telemetry.device_id,
                  sizeof(g_field_telemetry.device_id),
                  "ra8p1-cpkhmi");
    app_copy_text(g_field_telemetry.session_id,
                  sizeof(g_field_telemetry.session_id),
                  "local-boot");
    app_copy_text(g_field_telemetry.build_version,
                  sizeof(g_field_telemetry.build_version),
                  FSP_VERSION_STRING);

    app_command_cache_init(&g_field_command_cache);
    fault_record_queue_init(&g_field_fault_records);
    mission_plan_init_default(&g_field_mission_plan);
    status = mission_plan_validate(&g_field_mission_plan,
                                   APP_TELEMETRY_FOUNDATION_CAPABILITIES,
                                   &validation);
    if (APP_STATUS_OK != status)
    {
        return status;
    }

    status = app_telemetry_init(&g_field_telemetry_store, &g_field_telemetry);
    if (APP_STATUS_OK == status)
    {
        g_field_foundations_ready = true;
    }

    return status;
}

static app_status_t app_service_field_foundations (uint64_t now_us)
{
    camera_capture_diagnostics_t camera_diagnostics;
    wheat_ai_backend_info_t backend_info;

    if (!g_field_foundations_ready)
    {
        return APP_STATUS_NOT_INITIALIZED;
    }
    if (now_us < g_field_telemetry_deadline_us)
    {
        return APP_STATUS_OK;
    }

    g_field_telemetry_deadline_us = now_us + APP_FIELD_TELEMETRY_PERIOD_US;
    camera_capture_get_diagnostics(&camera_diagnostics);
    wheat_ai_backend_get_info(&backend_info);

    g_field_telemetry.sequence++;
    g_field_telemetry.monotonic_us = now_us;
    g_field_telemetry.camera_state = (uint8_t) camera_diagnostics.state;
    g_field_telemetry.ai_state = (uint8_t) g_ai_interaction.state;
    g_field_telemetry.network_state = (uint8_t) g_network_snapshot.network_state;
    g_field_telemetry.sd_state = storage_service_telemetry_state();
    if (g_storage_snapshot.mounted && g_storage_snapshot.writable)
    {
        g_field_telemetry.capabilities |= RA8P_CAPABILITY_SD_STORAGE;
    }
    else
    {
        g_field_telemetry.capabilities &= ~((uint32_t) RA8P_CAPABILITY_SD_STORAGE);
    }
    if (g_environment_ready &&
        (APP_STATUS_OK == environment_service_get_status(
            now_us, &g_environment_snapshot)))
    {
        g_field_telemetry.temperature_milli_c =
            g_environment_snapshot.dht11_reading.temperature_milli_c;
        g_field_telemetry.humidity_milli_percent =
            g_environment_snapshot.dht11_reading.humidity_milli_percent;
        g_field_telemetry.environment_valid =
            g_environment_snapshot.dht11_reading.has_value &&
            g_environment_snapshot.dht11_reading.current_sample_valid &&
            !g_environment_snapshot.dht11_reading.stale;
        g_field_telemetry.dht11_fault_code =
            (uint16_t) g_environment_snapshot.dht11_fault;
        g_field_telemetry.environment_age_ms =
            g_environment_snapshot.dht11_reading.age_ms;
        g_field_telemetry.light_raw =
            g_environment_snapshot.light_reading.raw;
        g_field_telemetry.light_average =
            g_environment_snapshot.light_reading.filtered_average;
        g_field_telemetry.light_tenths_percent =
            g_environment_snapshot.light_reading.calibrated_tenths_percent;
        g_field_telemetry.light_calibration_version =
            g_environment_snapshot.light_reading.calibration_version;
        g_field_telemetry.light_valid =
            g_environment_snapshot.light_reading.has_value &&
            g_environment_snapshot.light_reading.current_cycle_valid &&
            !g_environment_snapshot.light_reading.stale;
        g_field_telemetry.light_fault_code =
            (uint16_t) g_environment_snapshot.light_fault;
    }
    g_field_telemetry.fault_flags = (APP_STATUS_OK == g_last_status) ? 0U :
                                    APP_FIELD_FAULT_ACTIVE;
    if (0U < g_field_fault_records.dropped_oldest)
    {
        g_field_telemetry.fault_flags |= APP_FIELD_FAULT_HISTORY_LOST;
    }
    if ((0U < g_ai_interaction.completed_results) &&
        (APP_STATUS_OK == g_ai_status))
    {
        g_field_telemetry.wheat_count = (int32_t) g_ai_result.count;
        g_field_telemetry.inference_us = g_ai_total_us;
    }
    g_field_telemetry.mission_wheat_count = g_mission_wheat_count;
    g_field_telemetry.field_wheat_count = g_field_wheat_count;
    g_field_telemetry.counted_frame_count = g_counted_frame_count;
    app_copy_text(g_field_telemetry.model_version,
                  sizeof(g_field_telemetry.model_version),
                  backend_info.model_identity);

    return app_telemetry_publish(&g_field_telemetry_store, &g_field_telemetry);
}

static void app_service_environment (uint64_t now_us)
{
    bool const vision_timing_critical =
        (AI_INTERACTION_FREEZE_PENDING == g_ai_interaction.state) ||
        (AI_INTERACTION_INFERENCING == g_ai_interaction.state) ||
        g_ai_backend_active;
    app_status_t status;

    if (!g_environment_ready)
    {
        return;
    }

    if (!vision_timing_critical && (now_us >= g_dht11_deadline_us))
    {
        (void) environment_service_request_dht11(now_us);
        g_dht11_deadline_us = now_us + APP_DHT11_PERIOD_US;
    }
    if (!vision_timing_critical && (now_us >= g_light_deadline_us))
    {
        (void) environment_service_request_light(now_us);
        g_light_deadline_us = now_us + APP_LIGHT_PERIOD_US;
    }

    status = environment_service_process(now_us);
    if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
    {
        g_environment_status = status;
    }
    else
    {
        g_environment_status = APP_STATUS_OK;
    }
}

static void app_service_network (uint64_t now_us)
{
    app_status_t status;

    if (!g_network_ready)
    {
        if (now_us < g_network_init_deadline_us)
        {
            return;
        }
        g_network_status = esp_at_service_init(now_us);
        if (APP_STATUS_OK != g_network_status)
        {
            g_network_init_deadline_us = now_us + APP_NETWORK_INIT_RETRY_US;
            return;
        }
        g_network_ready = true;
        g_field_telemetry.capabilities |= RA8P_CAPABILITY_LAN_TCP;
    }

    status = esp_at_service_process(now_us);
    if ((APP_STATUS_OK != status) && (APP_STATUS_BUSY != status))
    {
        g_network_status = status;
    }
    else
    {
        g_network_status = APP_STATUS_OK;
    }
    esp_at_service_get_diagnostics(&g_network_snapshot);
    if (g_network_snapshot.state != g_network_last_logged_state)
    {
        g_network_last_logged_state = g_network_snapshot.state;
        if ((ESP_AT_SERVICE_STATE_CREDENTIALS_WAIT == g_network_snapshot.state) ||
            (ESP_AT_SERVICE_STATE_LISTENING == g_network_snapshot.state) ||
            (ESP_AT_SERVICE_STATE_BACKOFF == g_network_snapshot.state))
        {
            (void) printf("[network] state=%s wifi=%s ip=%s link=%u recovery=%lu status=%u\r\n",
                          esp_at_service_state_name(g_network_snapshot.state),
                          g_network_snapshot.wifi_connected ? "yes" : "no",
                          g_network_snapshot.got_ip ?
                              g_network_snapshot.station_ip : "none",
                          (unsigned int) g_network_snapshot.active_link_id,
                          (unsigned long) g_network_snapshot.recoveries,
                          (unsigned int) g_network_snapshot.last_status);
        }
    }
    if (g_network_last_telemetry_sequence != g_field_telemetry.sequence)
    {
        status = esp_at_service_queue_telemetry(&g_field_telemetry);
        if (APP_STATUS_OK == status)
        {
            g_network_last_telemetry_sequence = g_field_telemetry.sequence;
        }
    }
}

static void app_service_storage (uint64_t now_us)
{
    bool const vision_timing_critical =
        (AI_INTERACTION_FREEZE_PENDING == g_ai_interaction.state) ||
        (AI_INTERACTION_INFERENCING == g_ai_interaction.state) ||
        g_ai_backend_active;
    app_status_t status;

    if (!g_storage_ready)
    {
        return;
    }

    if (g_storage_last_telemetry_sequence != g_field_telemetry.sequence)
    {
        status = storage_service_enqueue_telemetry(&g_field_telemetry);
        if (APP_STATUS_OK == status)
        {
            g_storage_last_telemetry_sequence = g_field_telemetry.sequence;
        }
        else if (APP_STATUS_BUSY != status)
        {
            g_storage_status = status;
        }
    }

    status = storage_service_process(now_us, !vision_timing_critical);
    if ((APP_STATUS_OK == status) || (APP_STATUS_BUSY == status))
    {
        g_storage_status = APP_STATUS_OK;
    }
    else
    {
        g_storage_status = status;
    }
    storage_service_get_diagnostics(&g_storage_snapshot);
}

static void app_copy_text (char * p_destination,
                           size_t destination_bytes,
                           char const * p_source)
{
    size_t index = 0U;

    if ((NULL == p_destination) || (0U == destination_bytes))
    {
        return;
    }
    if (NULL != p_source)
    {
        while (((index + 1U) < destination_bytes) && ('\0' != p_source[index]))
        {
            p_destination[index] = p_source[index];
            index++;
        }
    }
    p_destination[index] = '\0';
}

static void app_service_led (uint64_t now_us)
{
    fsp_err_t io_err;

    if (now_us < g_led_deadline_us)
    {
        return;
    }

    g_led_deadline_us = now_us + APP_LED_PERIOD_US;
    g_led_on = !g_led_on;
    io_err = g_ioport.p_api->pinWrite(g_ioport.p_ctrl,
                                      APP_LED_PIN,
                                      g_led_on ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
    if (FSP_SUCCESS != io_err)
    {
        app_record_error(APP_STATUS_IO);
    }
}

static void app_service_heartbeat (uint64_t now_us)
{
    camera_capture_diagnostics_t diagnostics;
    platform_time_diagnostics_t time_diagnostics;
    lcd_display_diagnostics_t   lcd_diagnostics;
    lcd_i8080_bus_diagnostics_t lcd_bus_diagnostics;
    uart_console_diagnostics_t  uart_diagnostics;
    uint32_t                    capture_fps_x100 = 0U;
    uint32_t                    lcd_fps_x100 = 0U;

    if (now_us < g_heartbeat_deadline_us)
    {
        return;
    }

    g_heartbeat_deadline_us = now_us + APP_HEARTBEAT_PERIOD_US;
    g_heartbeat_count++;
    camera_capture_get_diagnostics(&diagnostics);
    platform_time_get_diagnostics(&time_diagnostics);
    lcd_display_service_get_diagnostics(&lcd_diagnostics);
    lcd_i8080_bus_get_diagnostics(&lcd_bus_diagnostics);
    uart_console_get_diagnostics(&uart_diagnostics);
    if (diagnostics.last_frame_interval_us > 0U)
    {
        uint64_t const scaled_fps = 100000000ULL / diagnostics.last_frame_interval_us;

        capture_fps_x100 = (scaled_fps > UINT32_MAX) ? UINT32_MAX : (uint32_t) scaled_fps;
    }
    if (now_us > g_last_heartbeat_us)
    {
        uint64_t const elapsed_us = now_us - g_last_heartbeat_us;
        uint32_t const frame_delta = lcd_diagnostics.completed_frames - g_last_lcd_frame_count;
        uint64_t const scaled_fps = ((uint64_t) frame_delta * 100000000ULL) / elapsed_us;

        lcd_fps_x100 = (scaled_fps > UINT32_MAX) ? UINT32_MAX : (uint32_t) scaled_fps;
    }
    if (((0U != (g_heartbeat_count & 1U)) ||
        (CAMERA_CAPTURE_STATE_ERROR == diagnostics.state)) &&
        (printf("[camera-hb] n=%lu led=%s state=%s done=%lu valid=%lu pub=%lu drop=%lu starve=%lu "
                "callbacks=%lu irq=0x%08lx streams=%lu resumes=%lu arms=%lu "
                "recover=%lu/%lu/%lu fail=%lu consecutive=%lu/%lu "
                "cdssr=%lu errors=%lu timeouts=%lu event=0x%08lx error=0x%08lx fsp=%ld "
                "regs=capsr:0x%08lx,cstsr:0x%08lx,cetcr:0x%08lx,ceier:0x%08lx "
                "interval_us=%lu/%lu/%lu fps=%lu.%02lu seq=%lu crc=0x%08lx "
                "crc_calc=%lu crc_errors=%lu crc_selftest_fail=%lu crc_init=%s "
                "crc_fsp=%ld crc_status=%u rearm_us=%lu/%lu process_us=%lu/%lu "
                "uart_queue=%lu/%lu uart_drop=%lu/%lu status=%u\r\n",
                (unsigned long) g_heartbeat_count,
                g_led_on ? "on" : "off",
                app_camera_state_name(diagnostics.state),
                (unsigned long) diagnostics.completed_frames,
                (unsigned long) diagnostics.valid_frames,
                (unsigned long) diagnostics.published_frames,
                (unsigned long) diagnostics.dropped_ready_frames,
                (unsigned long) diagnostics.capture_starved,
                (unsigned long) diagnostics.callback_count,
                (unsigned long) diagnostics.interrupt_mask,
                (unsigned long) diagnostics.stream_start_count,
                (unsigned long) diagnostics.stream_resume_count,
                (unsigned long) diagnostics.capture_start_count,
                (unsigned long) diagnostics.recoverable_errors,
                (unsigned long) diagnostics.recovery_rearms,
                (unsigned long) diagnostics.recovery_confirmed,
                (unsigned long) diagnostics.recovery_failures,
                (unsigned long) diagnostics.consecutive_recoverable_errors,
                (unsigned long) diagnostics.max_consecutive_recoverable_errors,
                (unsigned long) diagnostics.last_cdssr,
                (unsigned long) diagnostics.errors,
                (unsigned long) diagnostics.timeouts,
                (unsigned long) diagnostics.last_event_flags,
                (unsigned long) diagnostics.last_error_flags,
                (long) diagnostics.last_fsp_error,
                (unsigned long) diagnostics.last_capsr,
                (unsigned long) diagnostics.last_cstsr,
                (unsigned long) diagnostics.last_cetcr,
                (unsigned long) diagnostics.last_ceier,
                app_log_u64_us(diagnostics.last_frame_interval_us),
                app_log_u64_us(diagnostics.min_frame_interval_us),
                app_log_u64_us(diagnostics.max_frame_interval_us),
                (unsigned long) (capture_fps_x100 / 100U),
                (unsigned long) (capture_fps_x100 % 100U),
                (unsigned long) diagnostics.last_sequence,
                (unsigned long) diagnostics.last_crc32,
                (unsigned long) diagnostics.crc_calculations,
                (unsigned long) diagnostics.crc_errors,
                (unsigned long) diagnostics.crc_self_test_failures,
                diagnostics.crc_initialized ? "yes" : "no",
                (long) diagnostics.last_crc_driver_error,
                (unsigned int) diagnostics.last_crc_error_status,
                (unsigned long) diagnostics.last_rearm_us,
                (unsigned long) diagnostics.max_rearm_us,
                (unsigned long) diagnostics.last_process_us,
                (unsigned long) diagnostics.max_process_us,
                (unsigned long) uart_diagnostics.queued_bytes,
                (unsigned long) uart_diagnostics.queue_high_water,
                (unsigned long) uart_diagnostics.dropped_requests,
                (unsigned long) uart_diagnostics.dropped_bytes,
                (unsigned int) g_last_status) < 0))
    {
        app_record_error(APP_STATUS_IO);
    }
    else if ((0U == (g_heartbeat_count & 1U)) &&
             (CAMERA_CAPTURE_STATE_ERROR != diagnostics.state) &&
             (printf("[lcd-hb] n=%lu state=%s frames=%lu fps=%lu.%02lu skips=%lu slices=%lu "
                     "seq=%lu crc=0x%08lx frame_us=%lu/%lu slice_us=%lu/%lu hold_us=%lu/%lu "
                     "busy=%lu pixels=%lu bus_max_us=%lu errors=%lu status=%u\r\n",
                     (unsigned long) g_heartbeat_count,
                     lcd_display_service_state_name(lcd_diagnostics.state),
                     (unsigned long) lcd_diagnostics.completed_frames,
                     (unsigned long) (lcd_fps_x100 / 100U),
                     (unsigned long) (lcd_fps_x100 % 100U),
                     (unsigned long) lcd_diagnostics.skipped_frames,
                     (unsigned long) lcd_diagnostics.transfer_slices,
                     (unsigned long) lcd_diagnostics.last_sequence,
                     (unsigned long) lcd_diagnostics.last_crc32,
                     (unsigned long) lcd_diagnostics.last_frame_us,
                     (unsigned long) lcd_diagnostics.max_frame_us,
                     (unsigned long) lcd_diagnostics.last_slice_us,
                     (unsigned long) lcd_diagnostics.max_slice_us,
                     app_log_u64_us(diagnostics.last_consumer_hold_us),
                     app_log_u64_us(diagnostics.max_consumer_hold_us),
                     (unsigned long) lcd_diagnostics.acquire_busy,
                     (unsigned long) lcd_bus_diagnostics.pixels_written,
                     (unsigned long) lcd_bus_diagnostics.max_transfer_us,
                     (unsigned long) (lcd_diagnostics.errors + lcd_bus_diagnostics.errors),
                     (unsigned int) g_last_status) < 0))
    {
        app_record_error(APP_STATUS_IO);
    }

    if (0U == (g_heartbeat_count % APP_AI_HEARTBEAT_DIVISOR))
    {
        wheat_ai_backend_info_t backend_info;
        user_button_diagnostics_t button_diagnostics;
        uint32_t const snapshot_sequence = g_ai_snapshot_valid ? g_ai_snapshot_frame.sequence : 0U;
        uint32_t const snapshot_generation = g_ai_snapshot_valid ? g_ai_snapshot_frame.generation : 0U;

        wheat_ai_backend_get_info(&backend_info);
        user_button_get_diagnostics(&g_user_button, &button_diagnostics);
        if (printf("[ai-hb] n=%lu state=%s result_status=%u cycle_status=%u "
                   "key_press=%lu key_release=%lu key_irq=%lu key_consumed=%lu "
                   "key_pending=%lu key_ignored_irq=%lu key_ignored_busy=%lu "
                   "raw=%s stable=%s armed=%s "
                   "freeze=%lu inference=%lu results=%lu resumes=%lu busy=%lu "
                   "sdram_ready=%s sdram_status=%u backend=%s model=%s hash=%s "
                    "available=%s requests=%lu npu_invocations=%lu backend_status=%u "
                    "backend_active=%s tiles=%lu/12 "
                   "snapshot=%s address=0x%08lx generation=%lu sequence=%lu crc=0x%08lx "
                   "copies=%lu copy_us=%lu snapshot_errors=%lu acquire=%lu release=%lu "
                   "release_errors=%lu capture_policy=continuous-ping-pong "
                   "lcd_external=%s holds=%lu/%lu/%lu/%lu "
                   "pool_acquire=%lu pool_release=%lu stale_release=%lu\r\n",
                   (unsigned long) g_heartbeat_count,
                   ai_interaction_state_name(g_ai_interaction.state),
                   (unsigned int) g_ai_interaction.result_status,
                   (unsigned int) g_ai_status,
                   (unsigned long) button_diagnostics.press_events,
                   (unsigned long) button_diagnostics.release_events,
                   (unsigned long) button_diagnostics.irq_events,
                   (unsigned long) button_diagnostics.consumed_events,
                   (unsigned long) button_diagnostics.pending_press_events,
                   (unsigned long) button_diagnostics.ignored_irq_events,
                   (unsigned long) button_diagnostics.ignored_busy_events,
                   button_diagnostics.raw_pressed ? "pressed" : "released",
                   button_diagnostics.stable_pressed ? "pressed" : "released",
                   button_diagnostics.press_armed ? "yes" : "no",
                   (unsigned long) g_ai_interaction.freeze_requests,
                   (unsigned long) g_ai_interaction.inference_requests,
                   (unsigned long) g_ai_interaction.completed_results,
                   (unsigned long) g_ai_interaction.resume_requests,
                   (unsigned long) g_ai_interaction.busy_presses,
                   g_sdram_ready ? "yes" : "no",
                   (unsigned int) g_sdram_status,
                   backend_info.name,
                   backend_info.model_identity,
                   backend_info.model_hash,
                   backend_info.model_available ? "yes" : "no",
                   (unsigned long) backend_info.requests,
                    (unsigned long) backend_info.hardware_invocations,
                    (unsigned int) backend_info.last_status,
                    backend_info.frame_active ? "yes" : "no",
                    (unsigned long) backend_info.tiles_completed,
                    g_ai_snapshot_valid ? "sdram" : "none",
                   (unsigned long) (uintptr_t) g_ai_snapshot_storage,
                   (unsigned long) snapshot_generation,
                   (unsigned long) snapshot_sequence,
                   (unsigned long) (g_ai_snapshot_valid ? g_ai_snapshot_frame.crc32 : 0U),
                   (unsigned long) g_ai_snapshot_copies,
                   (unsigned long) g_ai_snapshot_copy_us,
                   (unsigned long) g_ai_snapshot_errors,
                   (unsigned long) g_ai_lease_acquires,
                   (unsigned long) g_ai_lease_releases,
                   (unsigned long) g_ai_lease_release_errors,
                   lcd_diagnostics.external_lease ? "yes" : "no",
                   (unsigned long) lcd_diagnostics.external_holds_started,
                   (unsigned long) lcd_diagnostics.external_holds_completed,
                   (unsigned long) lcd_diagnostics.external_hold_resumes,
                   (unsigned long) lcd_diagnostics.external_hold_failures,
                   (unsigned long) diagnostics.acquired_frames,
                   (unsigned long) diagnostics.released_frames,
                   (unsigned long) diagnostics.stale_releases) < 0)
        {
            app_record_error(APP_STATUS_IO);
        }
    }

    if (g_environment_ready && (0U == (g_heartbeat_count & 1U)))
    {
        (void) environment_service_get_status(now_us, &g_environment_snapshot);
        if (printf("[env-hb] n=%lu service=%u dht=%s temp_mc=%ld humidity_mpct=%lu "
                   "age_ms=%lu state=%s fault=%s ok=%lu/%lu noresp=%lu timeout=%lu "
                   "checksum=%lu hw=%lu light=%s raw=%u avg=%u pct_x10=%u cal=%u "
                   "age_ms=%lu state=%s fault=%s ok=%lu/%lu timeout=%lu adc=%lu hw=%lu\r\n",
                   (unsigned long) g_heartbeat_count,
                   (unsigned int) g_environment_status,
                   g_environment_snapshot.dht11_reading.current_sample_valid ? "valid" : "invalid",
                   (long) g_environment_snapshot.dht11_reading.temperature_milli_c,
                   (unsigned long) g_environment_snapshot.dht11_reading.humidity_milli_percent,
                   (unsigned long) g_environment_snapshot.dht11_reading.age_ms,
                   sensor_dht11_state_name(g_environment_snapshot.dht11_state),
                   sensor_dht11_fault_name(g_environment_snapshot.dht11_fault),
                   (unsigned long) g_environment_snapshot.dht11_diagnostics.successful_samples,
                   (unsigned long) g_environment_snapshot.dht11_diagnostics.requests,
                   (unsigned long) g_environment_snapshot.dht11_diagnostics.no_responses,
                   (unsigned long) g_environment_snapshot.dht11_diagnostics.capture_timeouts,
                   (unsigned long) g_environment_snapshot.dht11_diagnostics.checksum_errors,
                   (unsigned long) g_environment_snapshot.dht11_diagnostics.hardware_errors,
                   g_environment_snapshot.light_reading.current_cycle_valid ? "valid" : "invalid",
                   (unsigned int) g_environment_snapshot.light_reading.raw,
                   (unsigned int) g_environment_snapshot.light_reading.filtered_average,
                   (unsigned int) g_environment_snapshot.light_reading.calibrated_tenths_percent,
                   (unsigned int) g_environment_snapshot.light_reading.calibration_version,
                   (unsigned long) g_environment_snapshot.light_reading.age_ms,
                   sensor_light_state_name(g_environment_snapshot.light_state),
                   sensor_light_fault_name(g_environment_snapshot.light_fault),
                   (unsigned long) g_environment_snapshot.light_diagnostics.cycles_completed,
                   (unsigned long) g_environment_snapshot.light_diagnostics.cycles_requested,
                   (unsigned long) g_environment_snapshot.light_diagnostics.timeouts,
                   (unsigned long) g_environment_snapshot.light_diagnostics.adc_errors,
                   (unsigned long) g_environment_snapshot.light_diagnostics.hardware_errors) < 0)
        {
            app_record_error(APP_STATUS_IO);
        }
    }

    if (g_network_ready &&
        (0U == (g_heartbeat_count % APP_NETWORK_HEARTBEAT_DIVISOR)))
    {
        esp_at_service_get_diagnostics(&g_network_snapshot);
        if (printf("[net-hb] n=%lu service=%u state=%s network=%u wifi=%s ip=%s "
                   "listen=%s link=%u fw=%s cmd=%lu timeout=%lu error=%lu recover=%lu "
                   "ip_retry=%lu wifi_disc=%lu "
                   "rx=%lu/%lu/%lu tx=%lu/%lu send=%lu/%lu/%lu ipd=%lu/%lu "
                   "parser=%lu/%lu session=%lu/%lu snapshot=%lu/%lu/%u "
                   "telemetry_busy=%lu\r\n",
                   (unsigned long) g_heartbeat_count,
                   (unsigned int) g_network_status,
                   esp_at_service_state_name(g_network_snapshot.state),
                   (unsigned int) g_network_snapshot.network_state,
                   g_network_snapshot.wifi_connected ? "yes" : "no",
                   g_network_snapshot.got_ip ?
                       g_network_snapshot.station_ip : "none",
                   g_network_snapshot.server_listening ? "yes" : "no",
                   (unsigned int) g_network_snapshot.active_link_id,
                   (0U != g_network_snapshot.firmware_version[0]) ?
                       g_network_snapshot.firmware_version : "pending",
                   (unsigned long) g_network_snapshot.commands_sent,
                   (unsigned long) g_network_snapshot.command_timeouts,
                   (unsigned long) g_network_snapshot.command_errors,
                   (unsigned long) g_network_snapshot.recoveries,
                   (unsigned long) g_network_snapshot.ip_query_retries,
                   (unsigned long) g_network_snapshot.wifi_disconnects,
                   (unsigned long) g_network_snapshot.transport.rx_bytes,
                   (unsigned long) g_network_snapshot.transport.rx_high_water,
                   (unsigned long) g_network_snapshot.transport.rx_overflows,
                   (unsigned long) g_network_snapshot.transport.tx_bytes,
                   (unsigned long) g_network_snapshot.transport.tx_high_water,
                   (unsigned long) g_network_snapshot.send_requests,
                   (unsigned long) g_network_snapshot.send_completions,
                   (unsigned long) g_network_snapshot.send_failures,
                   (unsigned long) g_network_snapshot.ipd_bytes,
                   (unsigned long) g_network_snapshot.ignored_ipd_bytes,
                   (unsigned long) g_network_snapshot.parser.malformed_ipd,
                   (unsigned long) g_network_snapshot.parser.oversized_ipd,
                   (unsigned long) g_network_snapshot.session.legacy.frames_received,
                   (unsigned long) g_network_snapshot.session.legacy.protocol_errors,
                   (unsigned long) g_network_snapshot.session.legacy.snapshot_format_errors,
                   (unsigned long) g_network_snapshot.session.legacy.snapshot_encode_errors,
                   (unsigned int) g_network_snapshot.session.legacy.last_snapshot_status,
                   (unsigned long) g_network_snapshot.telemetry_busy) < 0)
        {
            app_record_error(APP_STATUS_IO);
        }
    }

    if (g_storage_ready &&
        (0U == (g_heartbeat_count % APP_STORAGE_HEARTBEAT_DIVISOR)))
    {
        sd_media_diagnostics_t media;

        storage_service_get_diagnostics(&g_storage_snapshot);
        sd_media_get_diagnostics(&media);
        if (printf("[sd-hb] n=%lu service=%u state=%u inserted=%s mounted=%s "
                   "writable=%s sectors=%lu queue=%u/%lu records=%lu last_seq=%lu "
                   "bytes=%lu/%lu mount=%lu/%lu media_err=%lu fs_err=%lu "
                   "recover=%lu/%lu transfer=%lu/%lu timeout=%lu fsp=%ld\r\n",
                   (unsigned long) g_heartbeat_count,
                   (unsigned int) g_storage_status,
                   (unsigned int) g_storage_snapshot.state,
                   media.media_inserted ? "yes" : "no",
                   g_storage_snapshot.mounted ? "yes" : "no",
                   g_storage_snapshot.writable ? "yes" : "no",
                   (unsigned long) media.sector_count,
                   (unsigned int) g_storage_snapshot.queued_records,
                   (unsigned long) g_storage_snapshot.queue_high_water,
                   (unsigned long) g_storage_snapshot.records_written,
                   (unsigned long) g_storage_snapshot.last_sequence,
                   (unsigned long) g_storage_snapshot.binary_bytes_written,
                   (unsigned long) g_storage_snapshot.csv_bytes_written,
                   (unsigned long) g_storage_snapshot.mount_attempts,
                   (unsigned long) g_storage_snapshot.mount_failures,
                   (unsigned long) g_storage_snapshot.media_failures,
                   (unsigned long) g_storage_snapshot.filesystem_failures,
                   (unsigned long) g_storage_snapshot.incomplete_tails_recovered,
                   (unsigned long) g_storage_snapshot.corrupt_tails_recovered,
                   (unsigned long) media.operations_completed,
                   (unsigned long) media.operation_errors,
                   (unsigned long) media.operation_timeouts,
                   (long) media.last_fsp_error) < 0)
        {
            app_record_error(APP_STATUS_IO);
        }
    }

    g_last_lcd_frame_count = lcd_diagnostics.completed_frames;
    g_last_heartbeat_us = now_us;
}

static unsigned long app_log_u64_us (uint64_t value)
{
    return (unsigned long) ((value > UINT32_MAX) ? UINT32_MAX : value);
}

static char const * app_camera_state_name (camera_capture_state_t state)
{
    static char const * const names[] =
    {
        "closed", "idle", "streaming", "stop-pending", "stopped", "error",
    };

    if ((uint32_t) state >= (uint32_t) (sizeof(names) / sizeof(names[0])))
    {
        return "unknown";
    }

    return names[state];
}

static void app_print_camera_summary (camera_sensor_test_result_t const * p_result)
{
    sccb_bus_diagnostics_t diagnostics;

    sccb_bus_get_diagnostics(&diagnostics);
    if (printf("[camera] id_test cycles=%lu completed=%lu passed=%lu id=0x%04x status=%s code=%u "
               "requests=%lu completions=%lu retries=%lu timeouts=%lu last_reg=0x%04x fsp=%ld event=%u\r\n",
               (unsigned long) p_result->cycles_requested,
               (unsigned long) p_result->cycles_completed,
               (unsigned long) p_result->cycles_passed,
               (unsigned int) p_result->chip_id,
               (APP_STATUS_OK == p_result->status) ? "ok" : "error",
               (unsigned int) p_result->status,
               (unsigned long) diagnostics.transfer_requests,
               (unsigned long) diagnostics.transfer_completions,
               (unsigned long) diagnostics.retries,
               (unsigned long) diagnostics.timeouts,
               (unsigned int) diagnostics.last_register,
               (long) diagnostics.last_fsp_error,
               (unsigned int) diagnostics.last_event) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }
}

static void app_print_mode_summary (app_status_t status)
{
    camera_sensor_diagnostics_t diagnostics;

    camera_sensor_get_diagnostics(&diagnostics);
    if (printf("[camera] mode=VGA version=%s format=RGB565-BE byte_order=big "
               "width=%u height=%u stride=%u fps=%u nominal_period_us=%lu readback=%ux%u "
               "pll=0x%02x/0x%02x/0x%02x/0x%02x root=0x%02x hts=%u vts=%u "
               "format_reg=0x%02x mux_reg=0x%02x stream_reg=0x%02x "
               "stream=%s table=%lu/%lu mismatches=%lu "
               "last_reg=0x%04x expected=0x%02x actual=0x%02x status=%s code=%u\r\n",
               CAMERA_SENSOR_MODE_VERSION,
               (unsigned int) CAMERA_SENSOR_VGA_WIDTH,
               (unsigned int) CAMERA_SENSOR_VGA_HEIGHT,
               (unsigned int) CAMERA_FRAME_STRIDE,
               (unsigned int) CAMERA_SENSOR_VGA_FRAME_RATE,
               (unsigned long) diagnostics.nominal_frame_period_us,
               (unsigned int) diagnostics.active_width,
               (unsigned int) diagnostics.active_height,
               (unsigned int) diagnostics.pll_control0,
               (unsigned int) diagnostics.pll_control1,
               (unsigned int) diagnostics.pll_control2,
               (unsigned int) diagnostics.pll_control3,
               (unsigned int) diagnostics.system_root_divider,
               (unsigned int) diagnostics.horizontal_total,
               (unsigned int) diagnostics.vertical_total,
               (unsigned int) diagnostics.format_control,
               (unsigned int) diagnostics.format_mux,
               (unsigned int) diagnostics.stream_control,
               diagnostics.streaming ? "on" : "off",
               (unsigned long) diagnostics.table_entries_written,
               (unsigned long) diagnostics.table_entry_count,
               (unsigned long) diagnostics.readback_mismatches,
               (unsigned int) diagnostics.last_register,
               (unsigned int) diagnostics.expected_value,
               (unsigned int) diagnostics.actual_value,
               (APP_STATUS_OK == status) ? "ok" : "error",
               (unsigned int) status) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }
}

static void app_print_ceu_summary (app_status_t status)
{
    camera_capture_diagnostics_t diagnostics;

    camera_capture_get_diagnostics(&diagnostics);
    if (printf("[camera] ceu=CEU0 bus=8 sensor=%ux%u samples=%ux%u bytes_per_sample=1 "
                "frame_bytes=%lu buffers=2 alignment=%u "
                "timeout_us=%lu irq=0x%08lx vsync_irq=off "
                "crc=CRC32-IEEE crc_init=%s crc_calc=%lu crc_errors=%lu "
                "crc_selftest_fail=%lu crc_fsp=%ld crc_status=%u status=%s code=%u fsp=%ld\r\n",
               (unsigned int) CAMERA_FRAME_WIDTH,
               (unsigned int) CAMERA_FRAME_HEIGHT,
               (unsigned int) CAMERA_FRAME_STRIDE,
               (unsigned int) CAMERA_FRAME_HEIGHT,
                (unsigned long) CAMERA_FRAME_BYTES,
                (unsigned int) CAMERA_FRAME_ALIGNMENT,
                (unsigned long) CAMERA_CAPTURE_TIMEOUT_US,
                (unsigned long) diagnostics.interrupt_mask,
                diagnostics.crc_initialized ? "yes" : "no",
                (unsigned long) diagnostics.crc_calculations,
                (unsigned long) diagnostics.crc_errors,
                (unsigned long) diagnostics.crc_self_test_failures,
                (long) diagnostics.last_crc_driver_error,
                (unsigned int) diagnostics.last_crc_error_status,
                (APP_STATUS_OK == status) ? "ok" : "error",
                (unsigned int) status,
               (long) diagnostics.last_fsp_error) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }
}

static void app_print_camera_signal_probe (void)
{
    app_camera_signal_probe_t probe = {0};
    uint32_t const cycles_per_us = SystemCoreClock / 1000000U;
    uint32_t const duration_cycles = APP_CAMERA_SIGNAL_PROBE_US * cycles_per_us;
    uint32_t const start_cycles = platform_time_raw_cycles();
    uint32_t previous_pclk = R_BSP_PinRead(BSP_IO_PORT_04_PIN_14);
    uint32_t previous_href = R_BSP_PinRead(BSP_IO_PORT_04_PIN_15);
    uint32_t previous_vsync = R_BSP_PinRead(BSP_IO_PORT_07_PIN_08);

    do
    {
        uint32_t const pclk = R_BSP_PinRead(BSP_IO_PORT_04_PIN_14);
        uint32_t const href = R_BSP_PinRead(BSP_IO_PORT_04_PIN_15);
        uint32_t const vsync = R_BSP_PinRead(BSP_IO_PORT_07_PIN_08);

        probe.samples++;
        probe.pclk_high_samples += pclk;
        probe.href_high_samples += href;
        probe.vsync_high_samples += vsync;
        probe.pclk_transitions += (pclk != previous_pclk) ? 1U : 0U;
        probe.href_transitions += (href != previous_href) ? 1U : 0U;
        probe.vsync_transitions += (vsync != previous_vsync) ? 1U : 0U;
        previous_pclk = pclk;
        previous_href = href;
        previous_vsync = vsync;
    } while ((platform_time_raw_cycles() - start_cycles) < duration_cycles);

    if (printf("[camera] signals duration_us=%lu samples=%lu pclk_edges=%lu href_edges=%lu vsync_edges=%lu\r\n",
               (unsigned long) APP_CAMERA_SIGNAL_PROBE_US,
               (unsigned long) probe.samples,
               (unsigned long) probe.pclk_transitions,
               (unsigned long) probe.href_transitions,
               (unsigned long) probe.vsync_transitions) < 0)
    {
        app_record_error(APP_STATUS_IO);
    }
}

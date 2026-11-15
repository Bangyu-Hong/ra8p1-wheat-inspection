$ErrorActionPreference = "Stop"

$cameraRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $PSScriptRoot ".build"
$gcc = "C:\msys64\ucrt64\bin\gcc.exe"

& (Join-Path $PSScriptRoot "check_fsp_generated_contract.ps1")

if (-not (Test-Path -LiteralPath $gcc)) {
    $gcc = (Get-Command gcc.exe -ErrorAction Stop).Source
}

# Avoid incompatible MinGW runtimes inherited from other developer tools.
$env:PATH = "$(Split-Path -Parent $gcc);C:\msys64\usr\bin;C:\Windows\system32;C:\Windows"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$common = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-Wfloat-equal",
    "-DPLATFORM_TIME_HOST_TEST",
    "-DCAMERA_FRAME_POOL_TEST",
    "-I$cameraRoot\src",
    "$cameraRoot\src\platform_time.c",
    "$cameraRoot\src\camera_frame_pool.c",
    "$PSScriptRoot\test_camera_core.c"
)

$testExe = Join-Path $buildDir "camera_core_tests.exe"
& $gcc @common "-o" $testExe
if ($LASTEXITCODE -ne 0) {
    throw "Host warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $testExe
if ($LASTEXITCODE -ne 0) {
    throw "Host tests failed with exit code $LASTEXITCODE"
}

$analyzerExe = Join-Path $buildDir "camera_core_tests_analyzer.exe"
& $gcc @common "-fanalyzer" "-o" $analyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Host -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $analyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built host tests failed with exit code $LASTEXITCODE"
}

$rgb565Exe = Join-Path $buildDir "lcd_rgb565_tests.exe"
$rgb565Test = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-I$cameraRoot\src",
    "$PSScriptRoot\test_lcd_rgb565.c"
)

& $gcc @rgb565Test "-o" $rgb565Exe
if ($LASTEXITCODE -ne 0) {
    throw "LCD RGB565 warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $rgb565Exe
if ($LASTEXITCODE -ne 0) {
    throw "LCD RGB565 exhaustive test failed with exit code $LASTEXITCODE"
}

$previewScalerExe = Join-Path $buildDir "lcd_preview_scaler_tests.exe"
$previewScalerAnalyzerExe = Join-Path $buildDir "lcd_preview_scaler_tests_analyzer.exe"
$previewScalerTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-Wfloat-equal",
    "-I$cameraRoot\src",
    "$cameraRoot\src\lcd_preview_scaler.c",
    "$PSScriptRoot\test_lcd_preview_scaler.c"
)

& $gcc @previewScalerTest "-o" $previewScalerExe
if ($LASTEXITCODE -ne 0) {
    throw "LCD preview scaler warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $previewScalerExe
if ($LASTEXITCODE -ne 0) {
    throw "LCD preview scaler tests failed with exit code $LASTEXITCODE"
}

& $gcc @previewScalerTest "-fanalyzer" "-o" $previewScalerAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "LCD preview scaler -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $previewScalerAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built LCD preview scaler tests failed with exit code $LASTEXITCODE"
}

$timingContractExe = Join-Path $buildDir "camera_lcd_timing_tests.exe"
$timingContractTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wshadow",
    "-I$cameraRoot\src",
    "$PSScriptRoot\test_camera_lcd_timing.c"
)

& $gcc @timingContractTest "-o" $timingContractExe
if ($LASTEXITCODE -ne 0) {
    throw "Camera/LCD timing contract compile failed with exit code $LASTEXITCODE"
}

& $timingContractExe
if ($LASTEXITCODE -ne 0) {
    throw "Camera/LCD timing contract test failed with exit code $LASTEXITCODE"
}

$uartQueueExe = Join-Path $buildDir "uart_console_queue_tests.exe"
$uartQueueAnalyzerExe = Join-Path $buildDir "uart_console_queue_tests_analyzer.exe"
$uartQueueTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-DUART_CONSOLE_HOST_TEST",
    "-I$PSScriptRoot\stubs",
    "-I$cameraRoot\src",
    "$cameraRoot\src\uart_console.c",
    "$PSScriptRoot\test_uart_console_queue.c"
)

& $gcc @uartQueueTest "-o" $uartQueueExe
if ($LASTEXITCODE -ne 0) {
    throw "UART console queue warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $uartQueueExe
if ($LASTEXITCODE -ne 0) {
    throw "UART console queue tests failed with exit code $LASTEXITCODE"
}

& $gcc @uartQueueTest "-fanalyzer" "-o" $uartQueueAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "UART console queue -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $uartQueueAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built UART console queue tests failed with exit code $LASTEXITCODE"
}

$espAtParserExe = Join-Path $buildDir "esp_at_parser_tests.exe"
$espAtParserAnalyzerExe = Join-Path $buildDir "esp_at_parser_tests_analyzer.exe"
$espAtParserTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-I$cameraRoot\src",
    "$cameraRoot\src\esp_at_parser.c",
    "$PSScriptRoot\test_esp_at_parser.c"
)

& $gcc @espAtParserTest "-o" $espAtParserExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT parser warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $espAtParserExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT parser tests failed with exit code $LASTEXITCODE"
}

& $gcc @espAtParserTest "-fanalyzer" "-o" $espAtParserAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT parser -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $espAtParserAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built ESP-AT parser tests failed with exit code $LASTEXITCODE"
}

$espAtTransportExe = Join-Path $buildDir "esp_at_transport_tests.exe"
$espAtTransportAnalyzerExe = Join-Path $buildDir "esp_at_transport_tests_analyzer.exe"
$espAtTransportTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-I$PSScriptRoot\stubs",
    "-I$cameraRoot\src",
    "$cameraRoot\src\esp_at_transport.c",
    "$PSScriptRoot\test_esp_at_transport.c"
)

& $gcc @espAtTransportTest "-o" $espAtTransportExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT transport warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $espAtTransportExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT transport tests failed with exit code $LASTEXITCODE"
}

& $gcc @espAtTransportTest "-fanalyzer" "-o" $espAtTransportAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT transport -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $espAtTransportAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built ESP-AT transport tests failed with exit code $LASTEXITCODE"
}

$localTcpSessionExe = Join-Path $buildDir "local_tcp_session_tests.exe"
$localTcpSessionAnalyzerExe = Join-Path $buildDir "local_tcp_session_tests_analyzer.exe"
$localTcpSessionTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-I$cameraRoot\src",
    "$cameraRoot\src\wire_protocol.c",
    "$cameraRoot\src\local_tcp_session.c",
    "$PSScriptRoot\test_local_tcp_session.c"
)

& $gcc @localTcpSessionTest "-o" $localTcpSessionExe
if ($LASTEXITCODE -ne 0) {
    throw "Local TCP session warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $localTcpSessionExe
if ($LASTEXITCODE -ne 0) {
    throw "Local TCP session tests failed with exit code $LASTEXITCODE"
}

& $gcc @localTcpSessionTest "-fanalyzer" "-o" $localTcpSessionAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Local TCP session -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $localTcpSessionAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built local TCP session tests failed with exit code $LASTEXITCODE"
}

$httpWebsocketExe = Join-Path $buildDir "http_websocket_protocol_tests.exe"
$httpWebsocketAnalyzerExe = Join-Path $buildDir "http_websocket_protocol_tests_analyzer.exe"
$httpWebsocketTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-I$cameraRoot\src",
    "$cameraRoot\src\http_protocol.c",
    "$cameraRoot\src\websocket_protocol.c",
    "$PSScriptRoot\test_http_websocket_protocol.c"
)

& $gcc @httpWebsocketTest "-o" $httpWebsocketExe
if ($LASTEXITCODE -ne 0) {
    throw "HTTP/WebSocket warning-as-error compile failed with exit code $LASTEXITCODE"
}
& $httpWebsocketExe
if ($LASTEXITCODE -ne 0) {
    throw "HTTP/WebSocket tests failed with exit code $LASTEXITCODE"
}

& $gcc @httpWebsocketTest "-fanalyzer" "-o" $httpWebsocketAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "HTTP/WebSocket -fanalyzer compile failed with exit code $LASTEXITCODE"
}
& $httpWebsocketAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built HTTP/WebSocket tests failed with exit code $LASTEXITCODE"
}

$lanSessionRouterExe = Join-Path $buildDir "lan_session_router_tests.exe"
$lanSessionRouterAnalyzerExe = Join-Path $buildDir "lan_session_router_tests_analyzer.exe"
$lanSessionRouterTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-I$cameraRoot\src",
    "$cameraRoot\src\wire_protocol.c",
    "$cameraRoot\src\local_tcp_session.c",
    "$cameraRoot\src\http_protocol.c",
    "$cameraRoot\src\websocket_protocol.c",
    "$cameraRoot\src\web_ui_asset.c",
    "$cameraRoot\src\lan_session_router.c",
    "$PSScriptRoot\test_lan_session_router.c"
)

& $gcc @lanSessionRouterTest "-o" $lanSessionRouterExe
if ($LASTEXITCODE -ne 0) {
    throw "LAN session router warning-as-error compile failed with exit code $LASTEXITCODE"
}
& $lanSessionRouterExe
if ($LASTEXITCODE -ne 0) {
    throw "LAN session router tests failed with exit code $LASTEXITCODE"
}

& $gcc @lanSessionRouterTest "-fanalyzer" "-o" $lanSessionRouterAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "LAN session router -fanalyzer compile failed with exit code $LASTEXITCODE"
}
& $lanSessionRouterAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built LAN session router tests failed with exit code $LASTEXITCODE"
}

$espAtServiceSources = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-I$PSScriptRoot\stubs",
    "-I$cameraRoot\src",
    "$cameraRoot\src\esp_at_transport.c",
    "$cameraRoot\src\esp_at_parser.c",
    "$cameraRoot\src\wire_protocol.c",
    "$cameraRoot\src\local_tcp_session.c",
    "$cameraRoot\src\http_protocol.c",
    "$cameraRoot\src\websocket_protocol.c",
    "$cameraRoot\src\web_ui_asset.c",
    "$cameraRoot\src\lan_session_router.c",
    "$cameraRoot\src\esp_at_service.c",
    "$PSScriptRoot\test_esp_at_service.c"
)

$espAtServiceNoCredentialsExe = Join-Path $buildDir "esp_at_service_no_credentials_tests.exe"
& $gcc @espAtServiceSources "-DESP_AT_NO_CREDENTIALS_HOST_TEST" "-o" $espAtServiceNoCredentialsExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT service no-credentials compile failed with exit code $LASTEXITCODE"
}
& $espAtServiceNoCredentialsExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT service no-credentials tests failed with exit code $LASTEXITCODE"
}

$espAtServiceCredentialsExe = Join-Path $buildDir "esp_at_service_credentials_tests.exe"
$espAtServiceCredentialsDefines = @(
    "-DESP_AT_CREDENTIALS_HOST_TEST"
)
& $gcc @espAtServiceSources @espAtServiceCredentialsDefines "-o" $espAtServiceCredentialsExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT service credentials compile failed with exit code $LASTEXITCODE"
}
& $espAtServiceCredentialsExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT service credentials tests failed with exit code $LASTEXITCODE"
}

$espAtServiceAnalyzerExe = Join-Path $buildDir "esp_at_service_tests_analyzer.exe"
& $gcc @espAtServiceSources @espAtServiceCredentialsDefines "-fanalyzer" "-o" $espAtServiceAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "ESP-AT service -fanalyzer compile failed with exit code $LASTEXITCODE"
}
& $espAtServiceAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built ESP-AT service tests failed with exit code $LASTEXITCODE"
}

$crc32Exe = Join-Path $buildDir "crc32_ieee_tests.exe"
$crc32AnalyzerExe = Join-Path $buildDir "crc32_ieee_tests_analyzer.exe"
$crc32Test = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-DCRC32_IEEE_HOST_TEST",
    "-I$cameraRoot\src",
    "$cameraRoot\src\crc32_ieee.c",
    "$PSScriptRoot\test_crc32_ieee.c"
)

& $gcc @crc32Test "-o" $crc32Exe
if ($LASTEXITCODE -ne 0) {
    throw "CRC32 IEEE warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $crc32Exe
if ($LASTEXITCODE -ne 0) {
    throw "CRC32 IEEE tests failed with exit code $LASTEXITCODE"
}

& $gcc @crc32Test "-fanalyzer" "-o" $crc32AnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "CRC32 IEEE -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $crc32AnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built CRC32 IEEE tests failed with exit code $LASTEXITCODE"
}

$fieldFoundationsExe = Join-Path $buildDir "field_foundations_tests.exe"
$fieldFoundationsAnalyzerExe = Join-Path $buildDir "field_foundations_tests_analyzer.exe"
$fieldFoundationsTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-I$cameraRoot\src",
    "$cameraRoot\src\wire_protocol.c",
    "$cameraRoot\src\mission_plan.c",
    "$cameraRoot\src\app_telemetry.c",
    "$cameraRoot\src\app_command.c",
    "$cameraRoot\src\fault_record.c",
    "$PSScriptRoot\test_field_foundations.c"
)

& $gcc @fieldFoundationsTest "-o" $fieldFoundationsExe
if ($LASTEXITCODE -ne 0) {
    throw "Field foundations warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $fieldFoundationsExe
if ($LASTEXITCODE -ne 0) {
    throw "Field foundations tests failed with exit code $LASTEXITCODE"
}

& $gcc @fieldFoundationsTest "-fanalyzer" "-o" $fieldFoundationsAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Field foundations -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $fieldFoundationsAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built field foundations tests failed with exit code $LASTEXITCODE"
}

$storageJournalExe = Join-Path $buildDir "storage_journal_tests.exe"
$storageJournalAnalyzerExe = Join-Path $buildDir "storage_journal_tests_analyzer.exe"
$storageJournalTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-I$cameraRoot\src",
    "$cameraRoot\src\wire_protocol.c",
    "$cameraRoot\src\storage_journal.c",
    "$PSScriptRoot\test_storage_journal.c"
)

& $gcc @storageJournalTest "-o" $storageJournalExe
if ($LASTEXITCODE -ne 0) {
    throw "Storage journal warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $storageJournalExe
if ($LASTEXITCODE -ne 0) {
    throw "Storage journal tests failed with exit code $LASTEXITCODE"
}

& $gcc @storageJournalTest "-fanalyzer" "-o" $storageJournalAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Storage journal -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $storageJournalAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built storage journal tests failed with exit code $LASTEXITCODE"
}

$sdMediaExe = Join-Path $buildDir "sd_media_tests.exe"
$sdMediaAnalyzerExe = Join-Path $buildDir "sd_media_tests_analyzer.exe"
$sdMediaTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-DSD_MEDIA_HOST_TEST",
    "-I$cameraRoot\src",
    "$cameraRoot\src\sd_media.c",
    "$PSScriptRoot\test_sd_media.c"
)

& $gcc @sdMediaTest "-o" $sdMediaExe
if ($LASTEXITCODE -ne 0) {
    throw "SD media warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $sdMediaExe
if ($LASTEXITCODE -ne 0) {
    throw "SD media tests failed with exit code $LASTEXITCODE"
}

& $gcc @sdMediaTest "-fanalyzer" "-o" $sdMediaAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "SD media -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $sdMediaAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built SD media tests failed with exit code $LASTEXITCODE"
}

$environmentSensorsExe = Join-Path $buildDir "environment_sensor_tests.exe"
$environmentSensorsAnalyzerExe = Join-Path $buildDir "environment_sensor_tests_analyzer.exe"
$environmentSensorsTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-I$cameraRoot\src",
    "$cameraRoot\src\sensor_dht11.c",
    "$cameraRoot\src\sensor_light.c",
    "$PSScriptRoot\test_environment_sensors.c"
)

& $gcc @environmentSensorsTest "-o" $environmentSensorsExe
if ($LASTEXITCODE -ne 0) {
    throw "Environment sensor warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $environmentSensorsExe
if ($LASTEXITCODE -ne 0) {
    throw "Environment sensor tests failed with exit code $LASTEXITCODE"
}

& $gcc @environmentSensorsTest "-fanalyzer" "-o" $environmentSensorsAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Environment sensor -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $environmentSensorsAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built environment sensor tests failed with exit code $LASTEXITCODE"
}

$aiFoundationsExe = Join-Path $buildDir "ai_foundations_tests.exe"
$aiFoundationsAnalyzerExe = Join-Path $buildDir "ai_foundations_tests_analyzer.exe"
$aiFoundationsTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-Wfloat-equal",
    "-I$cameraRoot\src",
    "$cameraRoot\src\user_button.c",
    "$cameraRoot\src\ai_interaction.c",
    "$cameraRoot\src\sdram_test.c",
    "$cameraRoot\src\wheat_preprocess.c",
    "$cameraRoot\src\wheat_postprocess.c",
    "$cameraRoot\src\wheat_yolox_decode.c",
    "$cameraRoot\src\wheat_ai_backend.c",
    "$cameraRoot\src\lcd_result_overlay.c",
    "$PSScriptRoot\test_ai_foundations.c"
)

& $gcc @aiFoundationsTest "-o" $aiFoundationsExe
if ($LASTEXITCODE -ne 0) {
    throw "AI foundations warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $aiFoundationsExe
if ($LASTEXITCODE -ne 0) {
    throw "AI foundations tests failed with exit code $LASTEXITCODE"
}

& $gcc @aiFoundationsTest "-fanalyzer" "-o" $aiFoundationsAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "AI foundations -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $aiFoundationsAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built AI foundations tests failed with exit code $LASTEXITCODE"
}

$lcdHoldExe = Join-Path $buildDir "lcd_hold_service_tests.exe"
$lcdHoldAnalyzerExe = Join-Path $buildDir "lcd_hold_service_tests_analyzer.exe"
$lcdHoldTest = @(
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wmissing-declarations",
    "-Wconversion",
    "-Wpointer-arith",
    "-Wshadow",
    "-Wlogical-op",
    "-Waggregate-return",
    "-Wfloat-equal",
    "-I$cameraRoot\src",
    "$PSScriptRoot\test_lcd_hold_service.c"
)

& $gcc @lcdHoldTest "-o" $lcdHoldExe
if ($LASTEXITCODE -ne 0) {
    throw "LCD hold service warning-as-error compile failed with exit code $LASTEXITCODE"
}

& $lcdHoldExe
if ($LASTEXITCODE -ne 0) {
    throw "LCD hold service tests failed with exit code $LASTEXITCODE"
}

& $gcc @lcdHoldTest "-fanalyzer" "-o" $lcdHoldAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "LCD hold service -fanalyzer compile failed with exit code $LASTEXITCODE"
}

& $lcdHoldAnalyzerExe
if ($LASTEXITCODE -ne 0) {
    throw "Analyzer-built LCD hold service tests failed with exit code $LASTEXITCODE"
}

Write-Output ("host-tests status=ok warning_compile=ok fanalyzer=ok crc32=ieee " +
              "rgb565_values=65536 preview=426x320 sensor_fps=25 lcd_nops=12 " +
              "field_protocol=ra8p-v1 mission=bounded telemetry=double-slot " +
              "environment=dht-async-light8-trimmed " +
               "storage=journal8-crc-config-ab-sdhi-dmac-fatfs-r015 " +
              "uart_queue=4096-wrap-owned-drop-bounded " +
              "esp_at=parser-ipd4-transport-request-owned-service-bounded tcp=single-session-priority " +
              "http=routes3-bounded websocket=rfc6455-masked-bounded lan=links5-lease-legacy " +
              "button=irq29-latch-release30ms sdram=patterns preprocess=generic " +
              "nms=bounded lcd_hold=app-owned")

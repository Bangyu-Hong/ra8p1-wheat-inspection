# ESP8266 Board HTTP/WebSocket Development And Board Gate

## Current Software State

The camera firmware owns ESP8266 through SCI0 on `P602/P603` and reset GPIO
`P306`. The bounded service now implements:

- hardware reset, `AT`, echo-off and firmware-version probing;
- station mode, `AT+CWJAP`, `AT+CIFSR`, `CIPMUX=1` and the unified HTTP/TCP server on port `80`;
- five bounded ESP link sessions, one automatic WebSocket control lease, read-only observers and
  `+IPD` parsing;
- `GET /`, `GET /health` and RFC 6455 `GET /ws` upgrade;
- reconnect state snapshot, incremental telemetry and ping/pong;
- bounded send timeouts, exponential reconnect backoff and periodic hard reset after repeated
  failures;
- offline latest-state caching so camera, LCD, AI and environment sampling do not wait for Wi-Fi.

The embedded browser page uses a bright responsive field-inspection layout with separate areas for
environment/count data, camera/AI status, chassis parameters, slide/capture parameters and system
diagnostics.  It configures a whole field by row count, row spacing, row travel and sampling
interval; there is no column-count input.  The page exposes the latest-frame count together with
mission and field cumulative counts.  A successful inference transaction is accumulated once, and
the current field total is explicitly a sum of valid non-overlapping samples until position-based
deduplication is added.

The serial-HMI fields remain reference input only.  The browser plan is the richer `MissionPlanV1`
surface and includes fixed-position versus multi-position slide modes, settle-before-capture,
inference timeout and the forward/backward slide jog.  Chassis, slide and remote vision action
buttons stay capability-gated; the current firmware still rejects WebSocket text commands as
`unsupported` until the application command parser and motion firmware are integrated, so the page
cannot accidentally drive unfinished hardware.

The board test is intentionally deferred while the hardware is powered off. Host tests cover the
state machine and protocol, but they do not prove RF association, DHCP, ESP8266 power integrity or
real TCP traffic.

## Local Credential Injection

Run this only on the local development computer:

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\workspace\camera\tools\configure_esp8266_credentials.ps1
```

The script prompts for a 2.4 GHz SSID and a hidden password. It writes
`workspace/camera/src/esp_at_credentials.h`, which is explicitly ignored by Git. It does not print
the password. Clear the local values with:

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\workspace\camera\tools\configure_esp8266_credentials.ps1 -Clear
```

This is a development bootstrap path. The strings are compiled into the SREC and are not encrypted.
The final field workflow will load validated configuration from persistent storage and accept a
screen-side Wi-Fi change request without putting credentials in source control or normal logs.

## Runtime Flow

```text
ignored local header -> RA8P1 firmware -> SCI0 AT+CWJAP -> ESP8266 -> 2.4 GHz AP
                     -> AT+CIFSR -> station IPv4 -> TCP server :80
                     -> computer / phone / tablet browser
                     -> STATE_SNAPSHOT -> TELEMETRY
```

The browser device must be on the same LAN. Open `http://<device-ip>/`, using the IPv4 address
printed by SCI9. No Windows client, Python environment or application installation is required. A
reconnect receives a complete `STATE_SNAPSHOT` before later telemetry.

## Deferred Board Gate

When the board is available:

1. Generate the ignored credentials locally, then complete Debug and Release Clean Builds.
2. Program only the reviewed combined Release SREC and capture its SHA-256.
3. Confirm SCI9 reports the ESP-AT version, Wi-Fi association, a non-zero IPv4 address and
   `state=listening`.
4. Open `http://<device-ip>/` and `http://<device-ip>/health`, then confirm the WebSocket receives a
   first `STATE_SNAPSHOT`.
5. Verify temperature, humidity, light, camera, AI and network fields update.
6. Connect two browsers, confirm only the first is the controller, close it, reconnect a browser and
   confirm the lease is reacquired. Then power-cycle or reset ESP8266 and verify automatic state
   synchronization.
7. Disconnect the access point and confirm bounded backoff while camera preview remains near 25 FPS.
8. Repeat USER freeze/inference/resume and confirm network activity does not change the vision flow.

Do not configure or enable SDHI hardware, serial-HMI or chassis integration until this gate has
passed or the network service has been explicitly disabled as a local degradation. Hardware-
independent storage record/queue tests may proceed offline because they cannot contend for pins,
interrupts or runtime bandwidth.

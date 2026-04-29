# ESP32 UDS Gateway — Project Skill

## Overview

ESP32-based automotive diagnostic gateway. Bridges CAN bus ↔ WebSocket so
a browser can send UDS (ISO 14229) requests and receive responses without
any additional hardware beyond the ESP32 + CAN transceiver.

**Author:** Abdennaby Ayyari  
**Target:** ESP32 (dual-core Xtensa LX6), ESP-IDF 5.5.x  
**Toolchain:** `idf.py` (ESP-IDF CMake build system)

---

## Architecture

```
Browser (WebSocket)
    │
    ▼
http_server  (/ws endpoint)
    │
ws_proto     parse JSON → uds_request_t / flash commands
    │
uds_service  ISO 14229 state machine (sessions, security, DID, flash)
    │
isotp_layer  ISO 15765-2 segmentation / reassembly
    │
twai_driver  ESP32 TWAI (CAN) hardware driver
    │
    ▼
CAN Bus → ECU
```

**Inter-task communication:** FreeRTOS queues only — no shared buffers.

| Queue | Item type | Depth | Direction |
|---|---|---|---|
| `q_twai_rx` | `twai_frame_t` | 32 | twai_task → isotp_task |
| `q_twai_tx` | `twai_frame_t` | 32 | isotp_task → twai_task |
| `q_isotp_rx` | `isotp_message_t` | 2 | isotp_task → uds_task |
| `q_isotp_tx` | `isotp_message_t` | 2 | uds_task → isotp_task |
| `q_uds_request` | `uds_request_t` | 2 | ws_proto → uds_task |
| `q_uds_response` | `uds_response_t` | 2 | uds_task → ws_proto_pump |
| `q_can_monitor` | `can_monitor_frame_t` | 16 | twai_task → ws_proto_pump |

**Task pinning:**

| Task | Core | Priority | Stack |
|---|---|---|---|
| `twai_task` | 1 | 10 | 4096 |
| `isotp_task` | 1 | 9 | 4096 |
| `uds_task` | 1 | 8 | 8192 |
| `tester_present_task` | 1 | 6 | 3072 |
| `ws_proto_pump_task` | 0 | 5 | 4096 |
| `arm_button_task` | 0 | 4 | 2048 |
| `led_task` | 0 | 3 | 4096 |
| WiFi (ESP-IDF internal) | 0 | — | — |

---

## Key Files

```
main/
  app_config.h       — all shared types (twai_frame_t, isotp_message_t,
                        uds_request_t, uds_response_t, can_monitor_frame_t)
                        and extern queue handles
  main.c             — queue creation, task creation, Wi-Fi init
  twai_driver.c      — CAN RX/TX loop; pushes to q_can_monitor for live monitor
                        (every frame RX and TX gets a can_monitor_frame_t)
  isotp_layer.c      — ISO-TP segmentation/reassembly + FC address logic
  uds_service.c      — UDS services: 0x10 0x11 0x14 0x19 0x22 0x23 0x27
                        0x2E 0x31 0x34-0x37 (0x3D handled browser-side only)
  ws_proto.c         — JSON parser (browser → firmware) + pump task
                        send_can_frame_json() pushes can_frame broadcasts
                        ws_proto_pump_task uses 20 ms timeout to drain
                        q_can_monitor between UDS responses
                        tx_id/rx_id validated ≤ 0x7FF before queuing (all
                        frames use standard 11-bit CAN via isotp_layer)
  http_server.c      — HTTP + WebSocket server; REST API: /api/config /api/status
                        /api/scan /api/wifi /api/reboot /api/factory-reset
  gw_nvs.c           — NVS helpers for persistent config (Wi-Fi, CAN IDs, bitrate)
  seed_to_key.c      — Security Access seed→key algorithm (customize per OEM)
  tester_present.c   — Automatic 0x3E keepalive during active sessions
  status_led.c       — RGB LED state machine (boot/wifi/ready/busy/error)
  arm_button.c       — Physical arm/disarm button with debounce

webui/
  index.html         — Single-page app (Console + Setup panes)
  app.js             — GatewayClient WebSocket class; UDS helpers; live CAN monitor;
                        ODIS Config Writer; ECU sidebar; Raw UDS send; ASC export
                        udsReq() throws on non-positive status, using r.message
                        from server when present (covers DTC/session/DID helpers)
  app.css            — Styles

sdkconfig.defaults   — Non-default Kconfig: 240 MHz, 1 kHz tick, TWAI ISR in IRAM,
                        WiFi pinned to Core 0, HTTP WS support, log level INFO
partitions.csv       — app(3 MB) + ota(3 MB) + nvs(24 KB) + storage(256 KB)
```

---

## Common Commands

```bash
# Full build
idf.py build

# Flash + monitor
idf.py -p COM3 flash monitor

# Only monitor (already flashed)
idf.py -p COM3 monitor

# Erase NVS (reset Wi-Fi / CAN config)
idf.py -p COM3 erase-region 0x9000 0x6000

# Reconfigure from scratch
rm sdkconfig && idf.py set-target esp32 && idf.py build

# Check heap after boot (look for "free heap" in monitor output)
# Minimum acceptable: ~80 KB free after HTTP server starts
```

---

## Critical Memory Constraints

`ISOTP_MAX_PAYLOAD = 4095` bytes. This makes several structs very large:

| Struct | Size |
|---|---|
| `isotp_message_t` | ~4108 B |
| `uds_request_t` | ~4136 B |
| `uds_response_t` | ~4132 B |

**Rule:** Never allocate these as local variables in any task function.
Always use `static` local or heap. Every function in `uds_service.c`,
`isotp_layer.c`, and `ws_proto.c` that touches these types uses `static`
locals to stay within task stack limits.

Stack overflow mode: `PTRVAL` (pattern-based) — catches overflows before
they corrupt adjacent heap.

**Queue memory budget** (depth × item size):
- `q_twai_rx/tx` (32 × 28 B): ~1.8 KB each
- `q_isotp_rx/tx` (2 × 4108 B): ~8 KB each
- `q_uds_request/response` (2 × ~4134 B): ~8 KB each
- `q_can_monitor` (16 × 24 B): ~384 B

Total queue heap: ~26 KB. HTTP server needs ~40 KB to start.

---

## WebSocket Protocol (JSON)

### Browser → ESP32

```jsonc
// Send UDS request (any SID — generic pass-through)
{"type":"uds_request","id":"abc123","tx_id":"0x7E0","rx_id":"0x7E8",
 "sid":"0x22","data":"F190","timeout_ms":2000}

// Start firmware flash sequence
{"type":"flash_start","id":"x","tx_id":"0x7E0","rx_id":"0x7E8",
 "size":131072,"crc32":1234567890,"address":"0x08000000",
 "sec_level":"0x01","erase_rid":"0xFF00","check_rid":"0xFF01"}

// Upload firmware chunk (base64)
{"type":"flash_upload_chunk","id":"x","offset":0,"data_b64":"AAEC..."}
```

`data` field is plain hex without `0x` prefix or spaces. SID is excluded
(firmware prepends it). Examples:
- SID 0x22, DID 0xF190 → `"sid":"0x22","data":"F190"`
- SID 0x2E, DID 0x04FF, value 0x01 → `"sid":"0x2E","data":"04FF01"`
- SID 0x3D WriteMemoryByAddress → `"sid":"0x3D","data":"24000001000002AABB..."` (browser-side only)

### ESP32 → Browser

```jsonc
// UDS response
{"type":"uds_response","id":"abc123","status":"positive",
 "sid":"0x62","data":"F190...","elapsed_ms":45}

// Live CAN frame (broadcast, no id) — emitted by send_can_frame_json()
{"type":"can_frame","ts":"12.345","dir":"RX","id":"7E8",
 "data":"06 62 F1 90 31 32 33","info":""}

// Flash progress
{"type":"flash_progress","phase":"erase","done":0,"total":131072}
```

`ts` in `can_frame` is seconds from device boot (float, 3 decimal places).
`dir` is `"RX"` or `"TX"` from the gateway's perspective.

---

## Console Tabs

| Tab | Function |
|---|---|
| **Overview** | Vehicle identity (VIN/Part/SW), Bus activity sparkline (live fps), DTC list, Raw UDS send widget |
| **Live** | All CAN frames real-time (RX+TX), filter by ID/data/direction, Export .ASC, pause/clear |
| **DTC** | Trouble codes read via SID 0x19 |
| **Flash** | Firmware upload (binary) via 0x34/0x36/0x37 sequence |
| **Config** | ODIS XML dataset writer (kalibrierung + parametrierung + service42) |

### ECU Sidebar
Clicking an ECU preset in the sidebar sets `canTxId`/`canRxId` inputs which
are read by `curIds()` for all console operations (Raw UDS, Read Vehicle, DTC).

Presets: ECM (0x7E0/0x7E8), Transmission (0x7E1/0x7E9),
ABS (0x7E2/0x7EA), Body Control (0x760/0x768).

### Raw UDS Widget (Overview tab)
Ad-hoc request from the browser: enter SID + hex data + timeout, press Send
or Enter. Response shown inline (positive with data, or NRC with name).
Uses `curIds()` — matches whichever ECU is active in the sidebar.

### ODIS Config Writer (Config tab)
Parses VW ODIS XML datasets from ODIS-Engineering / ODIS-S exports:
- `antwort_sg_konfig_kalibrierung.xml` → `GetAdjustCalibrationData`
  → N × SID 0x2E WriteDataByIdentifier (DID + calibration bytes)
- `antwort_sg_konfig_parametrierung.xml` → `GetParametrizeData`
  → param block write via SID 0x3D WriteMemoryByAddress or 0x34/0x36/0x37
- `antwort_sg_konfig_service42.xml` → `GetRepairAdvice` → metadata (VIN, change code)

Security: SID 0x27 seed→key flow. Seed shown in log; user enters computed
key in "Manual Key" field. LOGIN attribute in XML is informational (VW
workshop login code), not automatically sent.

### Export .ASC (Live tab)
Exports `FRAMES[]` in Vector ASC format (readable by CANalyzer, PEAK PCAN-View,
SavvyCAN, etc.):
```
date Fri Apr 25 10:30:00.000 2026
base hex  timestamps absolute
internal events logged
      12.345000 1  7E8             Rx   d 8 06 62 F1 90 31 32 33 00
```
Timestamps are seconds from `performance.now()` (relative to page load).

---

## Known Issues & Gotchas

- **FC (Flow Control) address**: uses `tx_state.tx_id` (not `rx_id - 8`).
  The OBD-II `-8` convention is a fallback only when no active TX state matches.

- **`uds_flash_sequence` mutex**: always use `FLASH_FAIL(err)` macro on
  error paths — it releases `g_uds_bus_lock` before returning. Never return
  directly without `xSemaphoreGive`.

- **`curIds()` in app.js**: reads `$('canTxId').value` / `$('canRxId').value`
  which are populated by `loadSetupConfig()` from `/api/config` at page load
  AND updated by sidebar ECU clicks. Defaults to `0x7E0`/`0x7E8` if empty.

- **`ws_proto_pump_task`** uses a 20 ms timeout (not `portMAX_DELAY`) so it
  can drain `q_can_monitor` between UDS responses.

- **Wi-Fi SSID provisioning**: configured via Setup pane → saved to NVS.
  No SSID → device boots but never connects (LED stays WIFI_CONNECTING).

- **`seed_to_key.c`**: stub implementation. Replace `seed_to_key_fn` with
  the OEM-specific algorithm before production deployment.

- **CAN ID validation in `ws_proto.c`**: `tx_id` and `rx_id` are rejected
  if > 0x7FF. `isotp_layer` always sets `extended = false`, so only 11-bit
  standard IDs are valid. Invalid IDs get an immediate `uds_response` or
  `flash_start_ack` error; the server's `"message"` field carries the reason.

- **`udsReq()` error propagation**: throws on any non-positive status using
  `r.message` first, then `NRC 0x<hex>`, then `Error: <status>`. All
  DTC/session helpers (`readDTCs`, `clearDTCs`, `startSession`, `readDID`)
  rely on this — do not add per-caller status checks after `udsReq()` calls.

- **SID 0x3D WriteMemoryByAddress**: handled entirely in browser JS (Config tab).
  The firmware `uds_task` forwards it to the ECU generically as a raw
  `uds_request`. No firmware change needed.

- **ODIS parametrization data format**: `0xFE,0xAA,0xBB,0xCC,...` is the
  raw byte content to write; `0xFE` is NOT a protocol opcode — it is data.

---

## REST API

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/config` | Returns JSON with `device`, `can`, `ws` sections |
| POST | `/api/config` | Saves `device.name`, `can.bitrate/tx_id/rx_id` to NVS |
| GET | `/api/status` | Wi-Fi, WS, flags (armed, bus_error, flash), system info |
| GET | `/api/scan` | Triggers Wi-Fi scan, returns network list |
| POST | `/api/wifi` | Saves SSID + password to NVS |
| POST | `/api/reboot` | Reboots ESP32 |
| POST | `/api/factory-reset` | Erases NVS, reboots |

---

## Hardware Defaults

| Signal | GPIO |
|---|---|
| CAN TX | 27 |
| CAN RX | 26 |
| Status LED | Kconfig (`CONFIG_GW_LED_GPIO`) |
| Arm button | Kconfig (`CONFIG_GW_ARM_BUTTON_GPIO`) |

Override via `idf.py menuconfig` → "ESP32 UDS Gateway".

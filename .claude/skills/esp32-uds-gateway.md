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
  isotp_layer.c      — ISO-TP segmentation/reassembly + FC address logic
  uds_service.c      — UDS services: 0x10 0x11 0x22 0x23 0x27 0x2E 0x31 0x34-0x37
  ws_proto.c         — JSON parser (browser → firmware) + pump task
                        (q_uds_response + q_can_monitor → WebSocket)
  http_server.c      — HTTP + WebSocket server; REST API: /api/config /api/status
                        /api/scan /api/wifi /api/reboot /api/factory-reset
  gw_nvs.c           — NVS helpers for persistent config (Wi-Fi, CAN IDs, bitrate)
  seed_to_key.c      — Security Access seed→key algorithm (customize per OEM)
  tester_present.c   — Automatic 0x3E keepalive during active sessions
  status_led.c       — RGB LED state machine (boot/wifi/ready/busy/error)
  arm_button.c       — Physical arm/disarm button with debounce

webui/
  index.html         — Single-page app (Console + Setup panes)
  app.js             — GatewayClient WebSocket class; UDS helpers; live CAN monitor
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
// Send UDS request
{"type":"uds_request","id":"abc123","tx_id":"0x7E0","rx_id":"0x7E8",
 "sid":"0x22","data":"F190","timeout_ms":2000}

// Start firmware flash sequence
{"type":"flash_start","id":"x","tx_id":"0x7E0","rx_id":"0x7E8",
 "size":131072,"crc32":1234567890,"address":"0x08000000",
 "sec_level":"0x01","erase_rid":"0xFF00","check_rid":"0xFF01"}

// Upload firmware chunk (base64)
{"type":"flash_upload_chunk","id":"x","offset":0,"data_b64":"AAEC..."}
```

### ESP32 → Browser

```jsonc
// UDS response
{"type":"uds_response","id":"abc123","status":"positive",
 "sid":"0x62","data":"F190...","elapsed_ms":45}

// Live CAN frame (broadcast, no id)
{"type":"can_frame","ts":"12.345","dir":"RX","id":"7E8",
 "data":"06 62 F1 90 31 32 33","info":""}

// Flash progress
{"type":"flash_progress","phase":"erase","done":0,"total":131072}
```

---

## Known Issues & Gotchas

- **FC (Flow Control) address**: uses `tx_state.tx_id` (not `rx_id - 8`).
  The OBD-II `-8` convention is a fallback only when no active TX state matches.

- **`uds_flash_sequence` mutex**: always use `FLASH_FAIL(err)` macro on
  error paths — it releases `g_uds_bus_lock` before returning. Never return
  directly without `xSemaphoreGive`.

- **`curIds()` in app.js**: reads `$('canTxId').value` / `$('canRxId').value`
  which are populated by `loadSetupConfig()` from `/api/config` at page load.
  Defaults to `0x7E0`/`0x7E8` if the inputs are empty.

- **`ws_proto_pump_task`** uses a 20 ms timeout (not `portMAX_DELAY`) so it
  can drain `q_can_monitor` between UDS responses.

- **Wi-Fi SSID provisioning**: configured via Setup pane → saved to NVS.
  No SSID → device boots but never connects (LED stays WIFI_CONNECTING).

- **`seed_to_key.c`**: stub implementation. Replace `seed_to_key_fn` with
  the OEM-specific algorithm before production deployment.

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

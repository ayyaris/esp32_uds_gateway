<div align="center">

<img src="https://raw.githubusercontent.com/espressif/esp-idf/master/docs/_static/espressif-logo.svg" width="80" alt="Espressif"/>

# ESP32 UDS Gateway

### Diagnostic & flashing bridge for vehicle ECUs

Real-time UDS over ISO-TP exposed as JSON via WebSocket. <br/>
Build your own OBD-II diagnostic tool — without a laptop tethered to the car.

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.2+-E7352C?style=flat-square&logo=espressif&logoColor=white)](https://docs.espressif.com/projects/esp-idf/)
[![Protocol](https://img.shields.io/badge/Protocol-UDS%20ISO%2014229-007AFF?style=flat-square)](https://www.iso.org/standard/72439.html)
[![Transport](https://img.shields.io/badge/Transport-ISO--TP%2015765--2-5E5CE6?style=flat-square)](https://www.iso.org/standard/66574.html)
[![CAN](https://img.shields.io/badge/CAN-2.0-34C759?style=flat-square)](#)
[![License](https://img.shields.io/badge/License-MIT-30D158?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-ESP32%20%7C%20S3%20%7C%20C6-666?style=flat-square)](#)

[**Quick Start**](#-quick-start) · [**Architecture**](#-architecture) · [**Protocol**](#-protocol) · [**Hardware**](#-hardware) · [**Safety**](#-safety--legal)

</div>

---

## ✨ What is this

A firmware for ESP32 that speaks to vehicle ECUs over CAN and exposes every
UDS service — including **firmware reflashing** — as a clean JSON WebSocket API.

Designed to sit between your web app and the car's OBD-II port. Your web
app never touches CAN timings directly; the ESP32 handles the hard real-time
parts locally so network jitter never corrupts a flash session.

```
 ┌─────────────┐        WebSocket + JSON        ┌──────────────┐
 │   Web App   │ ←──────────────────────────→  │   Backend     │
 └─────────────┘                                │   (proxy)     │
                                                └──────┬───────┘
                                                       │ WSS
                                                       ▼
                                                ┌──────────────┐
                                                │     ESP32    │      ┌────────┐
                                                │   Gateway    │─────▶│   ECU  │
                                                │  UDS/ISO-TP  │ CAN  │        │
                                                └──────────────┘      └────────┘
```

<br/>

## 🎯 Features

<table>
<tr>
<td width="50%">

### 🔬 Diagnostics
- Full UDS service set (0x10, 0x22, 0x27, 0x19, 0x2E, 0x31...)
- ISO-TP multi-frame RX & TX (FC, BS, STmin)
- Response Pending (NRC 0x78) with P2\* extension
- Automatic Tester Present during non-default sessions
- Live CAN frame streaming
- Per-ECU session management

</td>
<td width="50%">

### ⚡ Flashing
- Complete sequence orchestrated **locally** on ESP32
- RequestDownload → TransferData loop → Exit → Reset
- SecurityAccess with pluggable seed-to-key
- CRC32 verification before start
- Real-time progress updates to web app
- Firmware buffered in PSRAM (up to 4 MB)

</td>
</tr>
<tr>
<td width="50%">

### 🛡️ Safety
- Physical **arm button** required before flash write
- 30s auto-disarm window after press
- No inbound ports — outbound WebSocket only
- Bearer token + TLS by default
- Per-request correlation IDs
- Bus-off recovery & error counters

</td>
<td width="50%">

### 🎨 Developer UX
- Two polished web UIs (configuration + console)
- Python CLI client with REPL mode
- Local WebSocket bridge for dev
- **ECU simulator** firmware for a second ESP32
- Hardware schema with pinout & BOM
- Everything runs end-to-end without a vehicle

</td>
</tr>
</table>

<br/>

## 🚀 Quick Start

### 1 · Flash the firmware

```bash
git clone https://github.com/you/esp32-uds-gateway
cd esp32-uds-gateway/esp32_uds_gateway

. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py menuconfig   # Wi-Fi credentials, WS URI, auth token
idf.py build flash monitor
```

### 2 · Wire up the hardware

Minimal setup: ESP32 + CAN transceiver + OBD-II cable.

| ESP32      | SN65HVD230 | OBD-II (J1962) |
|------------|------------|----------------|
| 3V3        | VCC (1)    | —              |
| GND        | GND (2)    | Pin 5          |
| GPIO 21    | TXD (3)    | —              |
| GPIO 22    | RXD (4)    | —              |
| —          | CANH (7)   | Pin 6          |
| —          | CANL (6)   | Pin 14         |

Full schematic in [`docs/hardware_schema.html`](esp32_uds_gateway/docs/hardware_schema.html).

> ⚠️ **Do not** connect OBD pin 16 (+12V) to the ESP32. Power from USB.

### 3 · Talk to it

```bash
# start the local dev bridge
python tools/ws_bridge.py

# read the VIN
python tools/uds_test_client.py \
    --uri ws://localhost:8080/client \
    read-did --tx 0x7E0 --rx 0x7E8 --did 0xF190

# interactive REPL
python tools/uds_test_client.py \
    --uri ws://localhost:8080/client \
    repl --tx 0x7E0 --rx 0x7E8
```

<br/>

## 🏗 Architecture

Five FreeRTOS tasks with decreasing priorities. The CAN layer is hard
real-time, the network layer is best-effort — this is what lets a flash
session survive Wi-Fi jitter.

```
┌───────────────────────────────────────────────────────────────────┐
│                           ESP32 Firmware                          │
│                                                                   │
│  ┌─────────┐   ┌──────────┐   ┌──────────┐   ┌──────┐   ┌──────┐  │
│  │  TWAI   │   │  ISO-TP  │   │   UDS    │   │  TP  │   │  WS  │  │
│  │ prio 10 │──▶│  prio 9  │──▶│  prio 8  │──▶│  6   │──▶│  5   │  │
│  └────┬────┘   └────┬─────┘   └────┬─────┘   └──────┘   └──┬───┘  │
│       │             │               │                       │      │
│       │             │      ┌────────┴───────┐               │      │
│       │             │      │ flash_runner   │               │      │
│       │             │      │    prio 7      │               │      │
│       │             │      └────────────────┘               │      │
│       ▼             ▼                                       ▼      │
│    ┌─────────────────────┐                           ┌──────────┐  │
│    │   FreeRTOS queues   │                           │ arm_btn  │  │
│    └─────────────────────┘                           │ prio 4   │  │
│                                                      └──────────┘  │
└───────────────────────────────────────────────────────────────────┘
          │                                                   │
          ▼                                                   ▼
      CAN Bus                                           Wi-Fi / WSS
```

**Why the flash runner is separate.** `TransferData` blocks must ack within
P2* (~5s). If the web app were pacing the loop, one Wi-Fi glitch could
abort the session and leave the ECU in programming mode. So the ESP32 runs
the loop autonomously using `uds_request_blocking()` and only streams
progress events to the app.

<br/>

## 📡 Protocol

### UDS request / response

```jsonc
// request
{
  "id":         "req-001",
  "type":       "uds_request",
  "tx_id":      "0x7E0",
  "rx_id":      "0x7E8",
  "sid":        "0x22",        // ReadDataByIdentifier
  "data":       "F190",        // DID = VIN
  "timeout_ms": 1000
}

// response
{
  "id":         "req-001",
  "type":       "uds_response",
  "status":     "positive",
  "sid":        "0x62",
  "data":       "F190 5A 41 52 39 34 30 30 30 30 30 31 32 33 34 35 36 37",
  "elapsed_ms": 45
}
```

### Flash sequence

Three phases, all over the same WebSocket:

```
1. UPLOAD                     2. START                      3. MONITOR
───────────                   ────────                      ──────────
flash_upload_begin     ───▶   flash_start            ───▶   flash_progress × N
  { size, crc32 }               { address,                    { phase, done, total }
flash_upload_chunk × N            security_level,           flash_result
  { offset, data_b64 }            erase_before, ... }         { status: ok|error }
flash_upload_end
```

### Available services

| SID  | Name                         | Status |
|------|------------------------------|--------|
| 0x10 | DiagnosticSessionControl     | ✅     |
| 0x11 | ECUReset                     | ✅     |
| 0x14 | ClearDTC                     | ✅     |
| 0x19 | ReadDTCInformation           | ✅     |
| 0x22 | ReadDataByIdentifier         | ✅     |
| 0x27 | SecurityAccess               | ✅ *   |
| 0x28 | CommunicationControl         | ✅     |
| 0x2E | WriteDataByIdentifier        | ✅     |
| 0x31 | RoutineControl               | ✅     |
| 0x34 | RequestDownload              | ✅     |
| 0x36 | TransferData                 | ✅     |
| 0x37 | RequestTransferExit          | ✅     |
| 0x3E | TesterPresent                | ✅     |

<sub>* requires OEM-specific seed-to-key algorithm for real ECUs</sub>

<br/>

## 🔧 Hardware

### Bill of materials

| Component             | Reference            | Cost    | Notes                          |
|-----------------------|----------------------|---------|--------------------------------|
| Microcontroller       | ESP32-DevKitC        | ~€10    | or S3/C6 for more RAM/CAN-FD   |
| CAN transceiver       | SN65HVD230 breakout  | ~€3     | 3.3V native, no level shifting |
| OBD-II cable          | J1962 pigtail        | ~€8     | female to bare wires           |
| Momentary switch      | 6×6mm tactile        | €0.10   | already on most dev boards     |
| Termination resistor  | 120 Ω                | €0.05   | only for bench testing         |

**Total: ~€21** for a fully functional gateway.

### Bus topology

```
 ECU ─────┬────────────────────────┬───── ESP32 Gateway
          │          CAN Bus       │
        120Ω       (500 kbps)    120Ω
          │                        │
         GND                      GND
```

<br/>

## 🧪 Testing without a vehicle

The repo includes [`esp32_ecu_simulator`](esp32_ecu_simulator/) — a
second firmware that makes a spare ESP32 behave like an ECU. Flash it,
wire both boards to the same CAN bus, and every service (including the
full flash sequence) works end-to-end in ~2 minutes.

```bash
# board 1
cd esp32_ecu_simulator && idf.py build flash

# board 2
cd esp32_uds_gateway && idf.py build flash

# host
python tools/ws_bridge.py &
python tools/uds_test_client.py --uri ws://localhost:8080/client \
    flash --tx 0x7E0 --rx 0x7E8 --address 0x08010000 \
    --file test_firmware.bin
```

The simulator has a built-in seed-to-key (`key = rotL7(seed ^ 0xA5A5A5A5)`)
that matches the gateway's stub, so security unlock works out of the box.

<br/>

## 🎨 Web UIs

Two self-contained HTML files with zero external dependencies.

<table>
<tr>
<td width="50%" align="center">

**Configuration** · [`webui/config.html`](esp32_uds_gateway/webui/config.html)

macOS System Settings aesthetic <br/>
Served directly by the ESP32 in AP mode <br/>
Wi-Fi, gateway, CAN, security panes

</td>
<td width="50%" align="center">

**Diagnostics Console** · [`webui/console.html`](esp32_uds_gateway/webui/console.html)

Xcode Instruments aesthetic <br/>
Connects to backend via WebSocket <br/>
Overview · Live · DTC · Flash wizard

</td>
</tr>
</table>

Both auto-switch light/dark with `prefers-color-scheme`. Inline everything —
30 KB and 43 KB respectively, small enough to embed in flash.

<br/>

## 📂 Repository layout

```
esp32-uds-gateway/
├── esp32_uds_gateway/          ── main gateway firmware
│   ├── main/
│   │   ├── main.c              ── task bootstrap
│   │   ├── twai_driver.c       ── CAN layer
│   │   ├── isotp_layer.c       ── ISO-TP 15765-2
│   │   ├── uds_service.c       ── UDS services & flash orchestrator
│   │   ├── tester_present.c    ── automatic keepalive
│   │   ├── arm_button.c        ── physical write protection
│   │   ├── ws_client.c         ── WebSocket + JSON parsing
│   │   └── seed_to_key.c       ── OEM algorithm (replaceable)
│   ├── webui/
│   │   ├── config.html         ── settings UI
│   │   └── console.html        ── diagnostics UI
│   ├── tools/
│   │   ├── uds_test_client.py  ── CLI with REPL
│   │   └── ws_bridge.py        ── dev WebSocket bridge
│   └── docs/
│       └── hardware_schema.html
└── esp32_ecu_simulator/        ── second-board simulator for testing
    └── main/ecu_sim.c
```

<br/>

## 🗺 Roadmap

- [x] ISO-TP multi-frame RX & TX
- [x] Full UDS flash sequence
- [x] Tester Present auto-management
- [x] Arm button
- [x] Web UIs
- [x] ECU simulator
- [ ] CAN-FD support (MCP2518FD via SPI)
- [ ] Secure boot + flash encryption setup guide
- [ ] mTLS to backend
- [ ] DoIP (UDS over IP) bridge mode
- [ ] Pre-built OEM seed-to-key adapters

<br/>

## ⚠️ Safety & legal

Reprogramming vehicle ECUs is a serious operation with real consequences.

- **Bricking is possible.** A failed flash can leave an ECU unable to boot.
  Keep battery voltage stable (12.5V+), have a bench programmer ready.
- **Legal.** In the EU, tampering with emissions-related ECUs is regulated
  by Regulation (EU) 2018/858. In the US, EPA/CARB apply. Only reflash on
  vehicles you own or have explicit authorization to modify.
- **Authenticity.** Most modern ECUs use HSMs with signed firmware. Without
  cooperation from the manufacturer, you cannot write custom binaries —
  only restore official images.
- **Security.** The arm button is a last line of defense. Enable Secure
  Boot + Flash Encryption on any device you deploy outside a controlled
  environment.

This project is intended for **research, education, and legitimate
diagnostic work** on vehicles you have the right to service. Use
responsibly.

<br/>

## 🤝 Contributing

Pull requests welcome. A few areas that would really help:

- 🔌 **OEM seed-to-key adapters** (VAG, PSA, FCA — documented algorithms only)
- 🧪 **More simulator profiles** (different ECU types, failure modes)
- 🌐 **Real web app** for the console UI
- 📖 **DID database** for friendly diagnostics display

Open an issue first if you're planning something big.

<br/>

## 📜 License

MIT — do whatever, attribute, no warranty.

<br/>

<div align="center">

Made with ☕ and a lot of CAN frames.

⭐ Star if this helped you · 🐛 [Report a bug](../../issues) · 💬 [Discuss](../../discussions)

</div>

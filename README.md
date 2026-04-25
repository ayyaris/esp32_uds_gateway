# ESP32 UDS Gateway

Firmware ESP-IDF per gateway di diagnostica UDS/ISO-TP controllato via
WebSocket+JSON da una web app. Supporta:

- Richieste UDS singole (read/write DID, DTC, routine, ecc.)
- Sequenza completa di **riprogrammazione flash** eseguita localmente
  sull'ESP32 (nessun round-trip di rete nel loop TransferData)
- ISO-TP multi-frame in RX e TX con FC/BS/STmin
- Gestione Response Pending (P2*) e Tester Present (da abilitare)

## Architettura

```
  CAN Bus <---> [TWAI] <---> [ISO-TP] <---> [UDS] <---> [WS] <---> Backend
                 task         task          task        task
```

Tutte le comunicazioni tra task passano da FreeRTOS queue o API bloccanti
serializzate da mutex. Priorità decrescenti dal CAN verso la rete per
rispettare i timing UDS. Il flashing gira in un task dedicato (priorità 7)
e usa `isotp_send()` / `uds_request_blocking()` direttamente, senza code
verso la web app: ogni blocco `TransferData` viene inviato e atteso
localmente, evitando che il jitter WiFi causi timeout P2.

## Hardware

- ESP32 (classic, S3, C3, C6...) con PSRAM consigliata per firmware > 256KB
- Transceiver CAN: SN65HVD230 (3.3V), TJA1050/1051
- GPIO21 -> CAN_TX, GPIO22 -> CAN_RX (modificabili in `twai_driver.c`)

## Build

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32
idf.py menuconfig       # configura Wi-Fi, WS URI, token
idf.py build flash monitor
```

## Protocollo JSON

### Richiesta UDS generica

```json
{
  "id": "req-001",
  "type": "uds_request",
  "tx_id": "0x7E0",
  "rx_id": "0x7E8",
  "sid":   "0x22",
  "data":  "F190",
  "timeout_ms": 1000
}
```

Risposta:

```json
{
  "id": "req-001",
  "type": "uds_response",
  "status": "positive",
  "sid": "0x62",
  "data": "F190574643...",
  "elapsed_ms": 45
}
```

### Sequenza di flash

Flow completo:

1. **Upload firmware** in blocchi base64 (default 1-4 KB per chunk):

```json
{"type":"flash_upload_begin", "id":"up1", "size":131072, "crc32":3735928559}
{"type":"flash_upload_chunk", "id":"c1",  "offset":0,     "data_b64":"..."}
{"type":"flash_upload_chunk", "id":"c2",  "offset":1024,  "data_b64":"..."}
...
{"type":"flash_upload_end",   "id":"up1"}
```

Ogni messaggio riceve un ack `flash_upload_*_ack`. L'ESP32 verifica il
CRC32 prima di accettare lo start.

2. **Start flash**:

```json
{
  "type": "flash_start",
  "id": "flash1",
  "tx_id": "0x7E0",
  "rx_id": "0x7E8",
  "security_level": "0x01",
  "address": "0x08010000",
  "erase_before": true,
  "erase_routine_id": "0xFF00",
  "check_routine_id": "0xFF01"
}
```

3. **Progress** (async, dall'ESP32):

```json
{"type":"flash_progress", "phase":"session",          "done":0,     "total":0}
{"type":"flash_progress", "phase":"security",         "done":0,     "total":0}
{"type":"flash_progress", "phase":"erase",            "done":0,     "total":0}
{"type":"flash_progress", "phase":"request_download", "done":0,     "total":0}
{"type":"flash_progress", "phase":"transfer",         "done":4096,  "total":131072}
{"type":"flash_progress", "phase":"transfer",         "done":8192,  "total":131072}
...
{"type":"flash_progress", "phase":"transfer_exit",    "done":131072,"total":131072}
{"type":"flash_progress", "phase":"verify",           "done":0,     "total":0}
{"type":"flash_progress", "phase":"reset",            "done":0,     "total":0}
{"type":"flash_progress", "phase":"done",             "done":131072,"total":131072}
```

4. **Risultato**:

```json
{"type":"flash_result", "status":"ok", "code":0}
```

Codici errore (campo `code`): 0=OK, 1=session, 2=security, 3=erase,
4=request_download, 5=transfer, 6=exit, 7=check, 8=reset, 9=params.

## Seed-to-Key

Il file `seed_to_key.c` contiene uno **stub non funzionante** per ECU reali.
Sostituiscilo con l'algoritmo corretto del costruttore (ottenuto per via
legittima: licenza OEM, SDK ufficiale, pass-thru J2534 dealer). Senza
questo, SecurityAccess fallirà e la sequenza di flash si fermerà alla
fase "security".

## Cosa manca ancora

- **CAN-FD**: richiede MCP2518FD via SPI su ESP32 classic
- **Secure boot + flash encryption** per proteggere credenziali e firmware
  buffer da estrazione fisica
- **mTLS** verso backend (attualmente solo bearer token)
- **Algoritmo seed-to-key reale** del costruttore (lo stub funziona solo
  contro il simulatore ECU di test)

## Status LED (WS2812)

Il gateway pilota un LED RGB WS2812 su **GPIO4** per comunicare lo
stato del sistema a colpo d'occhio. Usa il driver RMT di ESP-IDF 5.x con
un encoder personalizzato per il protocollo NRZ a 800 kHz.

Palette colori e pattern:

| Stato | Colore | Pattern | Quando |
|-------|--------|---------|--------|
| BOOT / OFF | — | spento | durante init |
| WIFI_CONNECTING | ciano | respiro 2s | in attesa Wi-Fi |
| WS_CONNECTING | ciano | respiro 1s | Wi-Fi OK, attesa backend |
| NO_BACKEND | arancione | respiro 1.5s | Wi-Fi OK, WS disconnesso |
| AP_MODE | arancione | pulse 50% | modalità provisioning |
| READY | verde soft | fisso | tutto connesso, idle |
| UDS_ACTIVE | blu | fisso | sessione UDS non-default aperta |
| ARMED | giallo | pulse rapido | arm button premuto |
| SECURITY_UNLOCK | magenta | respiro 1.2s | seed/key exchange |
| ERASING | rosa-magenta | respiro 400ms | erase memory ECU |
| FLASHING | viola | heartbeat 2× | TransferData loop |
| BUS_OFF | rosso | pulse 200ms | CAN bus-off, recovery |
| FLASH_ERROR | rosso | fisso | ultimo flash fallito |
| FAULT_HARDWARE | rosso | SOS (... --- ...) | init error grave |

Sovrapposto agli stati principali, ogni frame CAN trasmesso o ricevuto
provoca un flash corto (~60ms): **blu per TX, viola per RX**. Questo
permette di vedere visivamente l'attività del bus senza sovrascrivere
lo stato operativo principale.

Il luminosity factor è `LED_BRIGHTNESS = 40/255` in `status_led.h`,
comodo in penombra ma non accecante. Modificalo se ti serve più intenso.

Alcuni dev kit (come ESP32-S3-DevKitC-1 o ESP32-C6-DevKitC-1) hanno già
un WS2812 on-board — verifica il pinout della tua board. Per altre
devkit collega il WS2812 esterno con: DIN → GPIO4, VCC → 5V (o 3.3V per
alcuni modelli, verifica datasheet), GND → GND.

## Arm button

Il gateway richiede la pressione di un pulsante fisico (default GPIO0 =
BOOT button) prima di accettare il comando `flash_start`. Questo
impedisce che la compromissione del backend basti a flashare un veicolo.

- **Pressione breve**: arm per 30 secondi, poi disarmo automatico
- **Pressione lunga (3+ sec)**: arm esteso senza timeout (uso da bench)
- Dopo ogni tentativo di flash (riuscito o fallito) l'arm viene consumato

Nella web app il messaggio `flash_start_ack` con `status=error` e
`message="device not armed..."` indica che serve premere il pulsante.

## Tools

Nella cartella `tools/` trovi:

- **uds_test_client.py**: CLI Python per testare il gateway via WebSocket.
  Supporta read-did, read-dtc, session, security, flash completo, REPL.
  `pip install websockets` e poi
  `python tools/uds_test_client.py --uri ws://... read-did --tx 0x7E0 --rx 0x7E8 --did 0xF190`

- **ws_bridge.py**: bridge WebSocket per sviluppo locale che serve anche
  la console.html staticamente.
  ```bash
  pip install websockets
  python tools/ws_bridge.py
  ```
  Poi apri `http://localhost:8080/` nel browser — la console è già
  collegata al bridge. L'ESP32 si connette a `ws://<host>:8080/gateway`
  e tutti i messaggi passano in real-time tra console e gateway.

## Docs

- **docs/hardware_schema.html**: schema elettrico completo con pinout
  SN65HVD230, OBD-II, terminazioni, note CAN-FD.

## Web UI

In `webui/` trovi l'applicazione web unificata, **tre file separati**:

```
webui/
├── index.html    (~12 KB, struttura)
├── app.css       (~22 KB, tutti gli stili)
└── app.js        (~21 KB, tutta la logica)
```

Un singolo `index.html` contiene entrambe le "app" (**Setup** e **Console**)
che si alternano via pulsante nella toolbar in alto. Questo permette
caching dei fogli di stile e del JS da parte del browser e riduce le
dimensioni totali rispetto a due file HTML monolitici.

**Setup app** — Stile macOS System Settings. Dialoga con l'ESP32 via REST:
- `GET  /api/status`       — stato wifi/ws/heap/uptime/mac
- `GET  /api/scan`         — scan Wi-Fi reale
- `POST /api/wifi`         — salva e applica credenziali
- `GET  /api/config`       — legge la config corrente (da NVS)
- `POST /api/config`       — salva config (NVS)
- `POST /api/reboot`       — riavvio soft
- `POST /api/factory-reset`— wipe NVS + reboot

Accessibile su `http://<ip-esp32>/` (porta 80).

**Console app** — Stile Xcode Instruments. Parla col backend/bridge
via WebSocket:
- Overview con letture VIN/Part/SW (DID 0xF190, 0xF187, 0xF189) e DTC
- Live view con log CAN in tempo reale + filtri (ID, direzione)
- Lista DTC per modulo con decodifica P/C/B/U
- Flash wizard completo: upload chunked base64 → flash_start →
  progress granulare → result

Riconnessione automatica con backoff esponenziale, correlazione
richiesta/risposta via ID, auto light/dark mode.

Tutti e tre i file vengono embeddati nel firmware (`EMBED_FILES` in
`main/CMakeLists.txt`) e serviti dall'`http_server.c` su endpoint
`/`, `/app.css`, `/app.js`.

## Test senza veicolo

Nel repository è incluso **esp32_ecu_simulator**, un simulatore ECU
completo per un secondo ESP32. Flashando il simulatore su un'altra board
collegata allo stesso bus CAN puoi provare tutto il flow end-to-end
(SecurityAccess, RequestDownload, TransferData, TransferExit, ECUReset)
senza rischi per una centralina reale.

L'algoritmo seed-to-key dello stub (`seed_to_key.c`) è volutamente
compatibile con il simulatore — XOR con 0xA5A5A5A5 + rotate-left 7 —
così il flash test passa al primo colpo.

### Workflow di test completo

1. Flash il simulatore su un ESP32:
   ```bash
   cd esp32_ecu_simulator && idf.py build flash
   ```
2. Flash il gateway sull'altro ESP32:
   ```bash
   cd esp32_uds_gateway && idf.py build flash
   ```
3. In un terminale, avvia il bridge WebSocket:
   ```bash
   python tools/ws_bridge.py
   ```
4. Configura il gateway per connettersi al bridge (URI in `app_config.h`
   o in `menuconfig`).
5. In un altro terminale, leggi il VIN:
   ```bash
   python tools/uds_test_client.py --uri ws://localhost:8080/client \
       read-did --tx 0x7E0 --rx 0x7E8 --did 0xF190
   ```
6. Per un flash test:
   ```bash
   # prepara un firmware fittizio
   dd if=/dev/urandom of=test_fw.bin bs=1024 count=16

   # premi il pulsante arm sull'ESP32 gateway
   # poi:
   python tools/uds_test_client.py --uri ws://localhost:8080/client \
       flash --tx 0x7E0 --rx 0x7E8 --address 0x08010000 \
       --file test_fw.bin --level 0x01
   ```

Alternativa: Linux + SocketCAN + `can-isotp` kernel module +
[python-udsoncan](https://github.com/pylessard/python-udsoncan).

## Disclaimer

La riprogrammazione di ECU veicolari ha implicazioni legali (UE
Reg. 2018/858 su manomissione emissioni, EPA/CARB negli USA) e
tecniche (flash fallito può richiedere bench programming per recupero).
Usa solo su veicoli di cui hai autorizzazione esplicita e tieni sempre
una strategia di recovery.

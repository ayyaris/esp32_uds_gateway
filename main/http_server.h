#pragma once
#include "app_config.h"
#include <stddef.h>
#include <stdbool.h>

/*
 * Embedded HTTP + WebSocket server.
 *
 * Endpoint REST:
 *   GET  /                   -> index.html (UI Setup + Console)
 *   GET  /app.css /app.js    -> asset statici
 *   GET  /api/status         -> stato (wifi, ws, can, uptime, heap, armed)
 *   GET  /api/scan           -> scan Wi-Fi (SSID, RSSI, secured)
 *   POST /api/wifi           -> salva credenziali e riconnette
 *   GET  /api/config         -> config corrente (CAN, device)
 *   POST /api/config         -> salva config (NVS)
 *   POST /api/reboot         -> riavvio
 *   POST /api/factory-reset  -> wipe namespace gw_cfg + reboot
 *
 * Endpoint WebSocket:
 *   GET  /ws                 -> protocollo UDS/flash JSON (vedi ws_proto.h)
 *
 * Tutto è locale all'ESP32: non esiste più un backend cloud intermedio.
 */

void http_server_start(void);
void http_server_stop(void);

/*
 * Invia un messaggio testo al client WebSocket attualmente connesso.
 * Ritorna true se inviato, false se nessun client. Thread-safe rispetto al
 * task http: usa httpd_ws_send_frame_async.
 */
bool http_server_ws_send_text(const char *data, size_t len);

/*
 * True se almeno un client WebSocket è connesso. Usato dal pump task per
 * evitare di serializzare risposte UDS quando nessuno le riceverà.
 */
bool http_server_ws_connected(void);

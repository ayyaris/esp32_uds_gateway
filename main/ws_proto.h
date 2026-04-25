#pragma once
#include "app_config.h"
#include <stddef.h>

/*
 * Protocollo WebSocket interno (browser <-> ESP32).
 *
 * Formato messaggi: JSON con campo "type". Vedi handle_json() per
 * l'elenco dei tipi supportati (uds_request, flash_upload_*, flash_start).
 *
 * Trasporto agnostico: questa libreria non sa come arrivano i bytes,
 * il chiamante (http_server WS endpoint) gli passa il payload via
 * ws_proto_handle_json(). Le risposte vengono serializzate e spedite via
 * http_server_ws_send_text() (definita in http_server.h).
 */

/* Parsifica e dispaccia un messaggio JSON ricevuto dal client. */
void ws_proto_handle_json(const char *payload, size_t len);

/*
 * Pump task: consuma q_uds_response e spedisce le risposte JSON al client
 * WebSocket connesso. Da avviare una sola volta in main.c.
 */
void ws_proto_pump_task(void *arg);

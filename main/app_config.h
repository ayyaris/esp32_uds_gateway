#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <stdint.h>
#include <stdbool.h>

/* ---------- Event group bits ---------- */
#define EVT_WIFI_CONNECTED     BIT0
#define EVT_WS_CONNECTED       BIT1
#define EVT_FLASH_IN_PROGRESS  BIT2
#define EVT_BUS_ERROR          BIT3

/* ---------- CAN bitrate ---------- */
typedef enum {
    CAN_BITRATE_250K,
    CAN_BITRATE_500K,   /* OBD-II standard */
    CAN_BITRATE_1M,
} can_bitrate_t;

/* ---------- Frame CAN raw ---------- */
typedef struct {
    uint32_t id;          /* 11 o 29 bit */
    bool     extended;    /* 29-bit ID */
    uint8_t  dlc;         /* 0..8 */
    uint8_t  data[8];
    uint64_t ts_us;       /* timestamp microsecondi */
} twai_frame_t;

/* ---------- Messaggio ISO-TP ricomposto ---------- */
#define ISOTP_MAX_PAYLOAD  4095   /* limite ISO 15765-2 classico */

typedef struct {
    uint32_t rx_id;       /* ID CAN sorgente (es. 0x7E8) */
    uint32_t tx_id;       /* ID CAN destinazione (es. 0x7E0) */
    uint16_t length;
    uint8_t  data[ISOTP_MAX_PAYLOAD];
} isotp_message_t;

/* ---------- Richiesta UDS dalla web app ---------- */
typedef struct {
    char     req_id[24];      /* correlazione richiesta/risposta */
    uint32_t tx_id;
    uint32_t rx_id;
    uint8_t  sid;             /* service identifier */
    uint8_t  payload[ISOTP_MAX_PAYLOAD];
    uint16_t payload_len;
    uint32_t timeout_ms;
} uds_request_t;

/* ---------- Risposta UDS verso web app ---------- */
typedef enum {
    UDS_STATUS_POSITIVE,
    UDS_STATUS_NEGATIVE,      /* NRC ricevuto */
    UDS_STATUS_TIMEOUT,
    UDS_STATUS_BUS_ERROR,
} uds_status_t;

typedef struct {
    char         req_id[24];
    uds_status_t status;
    uint8_t      sid;
    uint8_t      nrc;         /* negative response code se status==NEGATIVE */
    uint8_t      payload[ISOTP_MAX_PAYLOAD];
    uint16_t     payload_len;
    uint32_t     elapsed_ms;
} uds_response_t;

/* ---------- Frame CAN per il monitor live (WebSocket) ---------- */
typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
    bool     is_tx;    /* true = trasmesso dal gateway, false = ricevuto dal bus */
    uint64_t ts_us;
} can_monitor_frame_t;

/* ---------- Queue globali (definite in main.c) ---------- */
extern QueueHandle_t q_twai_rx;
extern QueueHandle_t q_twai_tx;
extern QueueHandle_t q_isotp_rx;
extern QueueHandle_t q_isotp_tx;
extern QueueHandle_t q_uds_request;
extern QueueHandle_t q_uds_response;
extern QueueHandle_t q_can_monitor;   /* can_monitor_frame_t -> ws_proto_pump_task */
extern EventGroupHandle_t app_events;

/*
 * Credenziali Wi-Fi, URI WebSocket e token auth vivono in NVS (namespace
 * "gw_cfg", popolato via http_server / UI di provisioning). I fallback
 * per il primo boot sono in Kconfig (menu "ESP32 UDS Gateway"). Vedi
 * gw_nvs.h per l'API di lettura.
 */

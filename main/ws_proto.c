/*
 * Protocollo WebSocket interno: parsing e gestione comandi UDS / flash
 * dal browser. Trasporto via http_server (endpoint /ws).
 */

#include "ws_proto.h"
#include "http_server.h"
#include "uds_service.h"
#include "arm_button.h"
#include "status_led.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crc.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "WSPROTO";

/* ------------------------------------------------------------------ */
/* Buffer firmware (PSRAM se disponibile)                             */
/* ------------------------------------------------------------------ */
static uint8_t *fw_buffer         = NULL;
static uint32_t fw_capacity       = 0;
static uint32_t fw_received       = 0;
static uint32_t fw_crc32_expected = 0;

static uint32_t flash_tx_id        = 0;
static uint32_t flash_rx_id        = 0;
static uint32_t flash_addr         = 0;
static uint8_t  flash_sec_level    = 0x01;
static uint32_t flash_erase_rid    = 0xFF00;
static uint32_t flash_check_rid    = 0xFF01;
static bool     flash_erase_before = true;

/* ------------------------------------------------------------------ */
/* Util parsing                                                       */
/* ------------------------------------------------------------------ */
static int hex2bin(const char *hex, uint8_t *out, int max_len)
{
    int hl = (int)strlen(hex);
    if (hl % 2 != 0 || hl / 2 > max_len) return -1;
    for (int i = 0; i < hl / 2; i++) {
        unsigned v;
        if (sscanf(&hex[i * 2], "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return hl / 2;
}

static void bin2hex(const uint8_t *in, int len, char *out)
{
    static const char *H = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = H[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = H[in[i] & 0x0F];
    }
    out[len * 2] = 0;
}

static uint32_t parse_hex_u32(const cJSON *item)
{
    if (!cJSON_IsString(item) || !item->valuestring) return 0;
    return (uint32_t)strtoul(item->valuestring, NULL, 0);
}

/* base64 decode (input ben formato da btoa() del browser) */
static int b64_decode(const char *in, int in_len, uint8_t *out, int out_cap)
{
    static const int8_t T[256] = {
        ['A']= 0,['B']= 1,['C']= 2,['D']= 3,['E']= 4,['F']= 5,['G']= 6,['H']= 7,
        ['I']= 8,['J']= 9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,
        ['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,['g']=32,['h']=33,
        ['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,
        ['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,['w']=48,['x']=49,
        ['y']=50,['z']=51,
        ['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,
        ['8']=60,['9']=61,['+']=62,['/']=63,
    };
    int o = 0, bits = 0, buf = 0;
    for (int i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '=' || c == ' ' || c == '\n' || c == '\r') continue;
        int v = T[(unsigned char)c];
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return -1;
            out[o++] = (buf >> bits) & 0xFF;
        }
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Send helpers (verso il client WS connesso)                          */
/* ------------------------------------------------------------------ */
static void send_json(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        http_server_ws_send_text(s, strlen(s));
        free(s);
    }
    cJSON_Delete(root);
}

static void send_ack(const char *id, const char *type, bool ok, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "id",     id ? id : "");
    cJSON_AddStringToObject(r, "type",   type);
    cJSON_AddStringToObject(r, "status", ok ? "ok" : "error");
    if (msg) cJSON_AddStringToObject(r, "message", msg);
    send_json(r);
}

static void flash_progress(const char *phase, uint32_t done, uint32_t total)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type",  "flash_progress");
    cJSON_AddStringToObject(r, "phase", phase);
    cJSON_AddNumberToObject(r, "done",  done);
    cJSON_AddNumberToObject(r, "total", total);
    send_json(r);
}

/* ------------------------------------------------------------------ */
/* Risposta UDS -> JSON                                               */
/* ------------------------------------------------------------------ */
static char *response_to_json(const uds_response_t *resp)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id",   resp->req_id);
    cJSON_AddStringToObject(root, "type", "uds_response");

    const char *st = "unknown";
    switch (resp->status) {
        case UDS_STATUS_POSITIVE:  st = "positive";  break;
        case UDS_STATUS_NEGATIVE:  st = "negative";  break;
        case UDS_STATUS_TIMEOUT:   st = "timeout";   break;
        case UDS_STATUS_BUS_ERROR: st = "bus_error"; break;
    }
    cJSON_AddStringToObject(root, "status", st);

    char hexbuf[16];
    snprintf(hexbuf, sizeof(hexbuf), "0x%02X", resp->sid);
    cJSON_AddStringToObject(root, "sid", hexbuf);

    if (resp->status == UDS_STATUS_NEGATIVE) {
        snprintf(hexbuf, sizeof(hexbuf), "0x%02X", resp->nrc);
        cJSON_AddStringToObject(root, "nrc", hexbuf);
    }
    if (resp->payload_len > 0) {
        char *h = malloc(resp->payload_len * 2 + 1);
        if (h) {
            bin2hex(resp->payload, resp->payload_len, h);
            cJSON_AddStringToObject(root, "data", h);
            free(h);
        } else {
            ESP_LOGW(TAG, "hex buffer alloc failed (%u bytes)",
                     (unsigned)(resp->payload_len * 2 + 1));
            cJSON_AddStringToObject(root, "data_error", "oom");
        }
    }
    cJSON_AddNumberToObject(root, "elapsed_ms", resp->elapsed_ms);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* ------------------------------------------------------------------ */
/* Handler comandi                                                    */
/* ------------------------------------------------------------------ */
static void handle_uds_request(const cJSON *root)
{
    const cJSON *j_id  = cJSON_GetObjectItem(root, "id");
    const cJSON *j_tx  = cJSON_GetObjectItem(root, "tx_id");
    const cJSON *j_rx  = cJSON_GetObjectItem(root, "rx_id");
    const cJSON *j_sid = cJSON_GetObjectItem(root, "sid");
    const cJSON *j_d   = cJSON_GetObjectItem(root, "data");
    const cJSON *j_to  = cJSON_GetObjectItem(root, "timeout_ms");

    uds_request_t req;
    memset(&req, 0, sizeof(req));
    if (cJSON_IsString(j_id)) {
        strncpy(req.req_id, j_id->valuestring, sizeof(req.req_id) - 1);
    }
    req.tx_id      = parse_hex_u32(j_tx);
    req.rx_id      = parse_hex_u32(j_rx);
    req.sid        = (uint8_t)parse_hex_u32(j_sid);
    req.timeout_ms = cJSON_IsNumber(j_to) ? (uint32_t)j_to->valuedouble : 1000;
    if (cJSON_IsString(j_d)) {
        int n = hex2bin(j_d->valuestring, req.payload, sizeof(req.payload));
        req.payload_len = n > 0 ? n : 0;
    }
    if (xQueueSend(q_uds_request, &req, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "q_uds_request full");
    }
}

static void handle_flash_upload_begin(const cJSON *root)
{
    const cJSON *j_id    = cJSON_GetObjectItem(root, "id");
    const cJSON *j_size  = cJSON_GetObjectItem(root, "size");
    const cJSON *j_crc   = cJSON_GetObjectItem(root, "crc32");

    if (!cJSON_IsNumber(j_size)) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_upload_begin_ack", false, "missing size");
        return;
    }
    uint32_t size = (uint32_t)j_size->valuedouble;

    if (fw_buffer) { heap_caps_free(fw_buffer); fw_buffer = NULL; }

    fw_buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fw_buffer) fw_buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!fw_buffer) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_upload_begin_ack", false, "alloc failed");
        return;
    }

    fw_capacity       = size;
    fw_received       = 0;
    fw_crc32_expected = cJSON_IsNumber(j_crc) ? (uint32_t)j_crc->valuedouble : 0;

    ESP_LOGI(TAG, "flash upload begin size=%lu", (unsigned long)size);
    send_ack(j_id ? j_id->valuestring : NULL,
             "flash_upload_begin_ack", true, NULL);
}

static void handle_flash_upload_chunk(const cJSON *root)
{
    const cJSON *j_id   = cJSON_GetObjectItem(root, "id");
    const cJSON *j_off  = cJSON_GetObjectItem(root, "offset");
    const cJSON *j_data = cJSON_GetObjectItem(root, "data_b64");

    if (!fw_buffer) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_upload_chunk_ack", false, "no buffer");
        return;
    }
    if (!cJSON_IsNumber(j_off) || !cJSON_IsString(j_data)) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_upload_chunk_ack", false, "bad args");
        return;
    }
    uint32_t off = (uint32_t)j_off->valuedouble;
    if (off >= fw_capacity) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_upload_chunk_ack", false, "offset overflow");
        return;
    }
    int n = b64_decode(j_data->valuestring, strlen(j_data->valuestring),
                       &fw_buffer[off], fw_capacity - off);
    if (n < 0) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_upload_chunk_ack", false, "decode fail");
        return;
    }
    fw_received = off + n;
    send_ack(j_id ? j_id->valuestring : NULL,
             "flash_upload_chunk_ack", true, NULL);
}

static void handle_flash_upload_end(const cJSON *root)
{
    const cJSON *j_id = cJSON_GetObjectItem(root, "id");
    if (!fw_buffer || fw_received != fw_capacity) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_upload_end_ack", false, "incomplete");
        return;
    }
    uint32_t crc = esp_crc32_le(0, fw_buffer, fw_received);
    if (fw_crc32_expected && crc != fw_crc32_expected) {
        ESP_LOGE(TAG, "CRC mismatch got=0x%08lX exp=0x%08lX",
                 (unsigned long)crc, (unsigned long)fw_crc32_expected);
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_upload_end_ack", false, "crc mismatch");
        return;
    }
    ESP_LOGI(TAG, "flash upload complete size=%lu crc=0x%08lX",
             (unsigned long)fw_received, (unsigned long)crc);
    send_ack(j_id ? j_id->valuestring : NULL,
             "flash_upload_end_ack", true, NULL);
}

static TaskHandle_t s_flash_task = NULL;

static void flash_runner_task(void *arg)
{
    uds_flash_params_t p = {
        .tx_id            = flash_tx_id,
        .rx_id            = flash_rx_id,
        .security_level   = flash_sec_level,
        .memory_address   = flash_addr,
        .memory_size      = fw_received,
        .address_len      = 4,
        .size_len         = 4,
        .max_block_size   = 0,
        .erase_before     = flash_erase_before,
        .erase_routine_id = flash_erase_rid,
        .check_routine_id = flash_check_rid,
        .firmware         = fw_buffer,
        .firmware_len     = fw_received,
        .progress_cb      = flash_progress,
    };

    flash_result_t r = uds_flash_sequence(&p);
    arm_button_consume();

    if (r == FLASH_OK) led_set_state(LED_STATE_READY);
    else               led_set_state(LED_STATE_FLASH_ERROR);

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type",   "flash_result");
    cJSON_AddStringToObject(msg, "status", r == FLASH_OK ? "ok" : "error");
    cJSON_AddNumberToObject(msg, "code",   r);
    send_json(msg);

    s_flash_task = NULL;
    vTaskDelete(NULL);
}

static void handle_flash_start(const cJSON *root)
{
    const cJSON *j_id = cJSON_GetObjectItem(root, "id");
    if (!fw_buffer || fw_received == 0) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_start_ack", false, "no firmware loaded");
        return;
    }
    if (!arm_button_is_armed()) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_start_ack", false,
                 "device not armed: press arm button on ESP32");
        ESP_LOGW(TAG, "flash_start refused: not armed");
        return;
    }
    if (s_flash_task) {
        send_ack(j_id ? j_id->valuestring : NULL,
                 "flash_start_ack", false, "flash already in progress");
        return;
    }
    flash_tx_id        = parse_hex_u32(cJSON_GetObjectItem(root, "tx_id"));
    flash_rx_id        = parse_hex_u32(cJSON_GetObjectItem(root, "rx_id"));
    flash_addr         = parse_hex_u32(cJSON_GetObjectItem(root, "address"));
    flash_sec_level    = (uint8_t)parse_hex_u32(cJSON_GetObjectItem(root, "security_level"));
    flash_erase_rid    = parse_hex_u32(cJSON_GetObjectItem(root, "erase_routine_id"));
    flash_check_rid    = parse_hex_u32(cJSON_GetObjectItem(root, "check_routine_id"));
    const cJSON *j_eb  = cJSON_GetObjectItem(root, "erase_before");
    flash_erase_before = cJSON_IsBool(j_eb) ? cJSON_IsTrue(j_eb) : true;

    send_ack(j_id ? j_id->valuestring : NULL,
             "flash_start_ack", true, "flash started");

    xTaskCreatePinnedToCore(flash_runner_task, "flash", 8192, NULL, 7,
                            &s_flash_task, 1);
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                         */
/* ------------------------------------------------------------------ */
void ws_proto_handle_json(const char *payload, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) { ESP_LOGW(TAG, "JSON parse fail"); return; }
    const cJSON *j_type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(j_type)) { cJSON_Delete(root); return; }

    const char *t = j_type->valuestring;
    if      (strcmp(t, "uds_request")        == 0) handle_uds_request(root);
    else if (strcmp(t, "flash_upload_begin") == 0) handle_flash_upload_begin(root);
    else if (strcmp(t, "flash_upload_chunk") == 0) handle_flash_upload_chunk(root);
    else if (strcmp(t, "flash_upload_end")   == 0) handle_flash_upload_end(root);
    else if (strcmp(t, "flash_start")        == 0) handle_flash_start(root);
    else if (strcmp(t, "ping")               == 0) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "type", "pong");
        const cJSON *j_id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(j_id)) cJSON_AddStringToObject(r, "id", j_id->valuestring);
        send_json(r);
    }
    else ESP_LOGW(TAG, "unknown type: %s", t);

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* Frame CAN raw -> JSON per il monitor live                          */
/* ------------------------------------------------------------------ */
static void send_can_frame_json(const can_monitor_frame_t *f)
{
    /* ts: uptime in secondi con 3 decimali (es. "12.345") */
    char ts[16];
    snprintf(ts, sizeof(ts), "%.3f", (double)f->ts_us / 1e6);

    /* id: hex senza 0x (es. "7E8") */
    char id_str[12];
    snprintf(id_str, sizeof(id_str), "%X", (unsigned)f->id);

    /* data: byte separati da spazio (es. "02 10 03"), max 8 byte DLC */
    char data_str[8 * 3 + 1]; /* "XX XX XX XX XX XX XX XX\0" = 24+1 */
    char *p = data_str;
    for (int i = 0; i < f->dlc; i++) {
        if (i) *p++ = ' ';
        p += sprintf(p, "%02X", f->data[i]);
    }
    *p = 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "can_frame");
    cJSON_AddStringToObject(root, "ts",   ts);
    cJSON_AddStringToObject(root, "dir",  f->is_tx ? "TX" : "RX");
    cJSON_AddStringToObject(root, "id",   id_str);
    cJSON_AddStringToObject(root, "data", data_str);
    cJSON_AddStringToObject(root, "info", "");
    send_json(root);
}

/* ------------------------------------------------------------------ */
void ws_proto_pump_task(void *arg)
{
    (void)arg;
    /* Static: sizeof(uds_response_t) ~4132 B, task stack is only 4096 B */
    static uds_response_t      resp;
    static can_monitor_frame_t cf;
    while (1) {
        /* Attendi risposta UDS con timeout breve per poter servire anche
         * il monitor CAN senza latenza eccessiva (20 ms max). */
        if (xQueueReceive(q_uds_response, &resp, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (http_server_ws_connected()) {
                char *json = response_to_json(&resp);
                if (json) {
                    http_server_ws_send_text(json, strlen(json));
                    free(json);
                }
            }
        }
        /* Svuota tutti i frame CAN in coda (non bloccante) */
        if (http_server_ws_connected()) {
            while (xQueueReceive(q_can_monitor, &cf, 0) == pdTRUE) {
                send_can_frame_json(&cf);
            }
        } else {
            /* Nessun client connesso: svuota la coda per evitare accumulo */
            while (xQueueReceive(q_can_monitor, &cf, 0) == pdTRUE) {}
        }
    }
}

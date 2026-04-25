/*
 * HTTP server: serve config.html e REST API.
 */

#include "http_server.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <ctype.h>
#include "app_config.h"
#include "arm_button.h"
#include "ws_proto.h"
#include "freertos/semphr.h"
#include <unistd.h>

static const char *TAG = "HTTP";

static httpd_handle_t server = NULL;

/* ------------------------------------------------------------------ */
/* Stato WebSocket                                                    */
/* ------------------------------------------------------------------ */
/*
 * Single-client model: si tiene il fd dell'ultimo browser che ha aperto
 * /ws. Se un secondo client si connette, sostituisce il primo. È
 * sufficiente per uso da ufficina / banco.
 */
static int            g_ws_fd     = -1;
static httpd_handle_t g_ws_server = NULL;
static SemaphoreHandle_t g_ws_mtx = NULL;

/* Embedded asset files (configurati in CMakeLists con EMBED_FILES) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t app_css_start[]    asm("_binary_app_css_start");
extern const uint8_t app_css_end[]      asm("_binary_app_css_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");

/* ------------------------------------------------------------------ */
/* Util                                                               */
/* ------------------------------------------------------------------ */
static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    cJSON_Delete(obj);
    return r;
}

static void str_trim(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static esp_err_t send_error(httpd_req_t *req, int code, const char *msg)
{
    httpd_resp_set_status(req, code == 400 ? "400 Bad Request" :
                               code == 404 ? "404 Not Found"   :
                               code == 500 ? "500 Server Error": "500 Error");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", msg);
    return send_json(req, r);
}

static char *read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) return NULL;
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[total] = 0;
    return buf;
}

/* ------------------------------------------------------------------ */
/* NVS helpers                                                        */
/* ------------------------------------------------------------------ */
#define NVS_NS "gw_cfg"

static esp_err_t nvs_set_str_safe(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_get_str_default(const char *key, char *out,
                                     size_t out_sz, const char *def)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        strncpy(out, def, out_sz - 1); out[out_sz - 1] = 0;
        return ESP_OK;
    }
    size_t sz = out_sz;
    esp_err_t err = nvs_get_str(h, key, out, &sz);
    if (err != ESP_OK) { strncpy(out, def, out_sz - 1); out[out_sz - 1] = 0; }
    nvs_close(h);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Handlers                                                           */
/* ------------------------------------------------------------------ */
static esp_err_t root_handler(httpd_req_t *req)
{
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t css_handler(httpd_req_t *req)
{
    size_t len = app_css_end - app_css_start;
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    return httpd_resp_send(req, (const char *)app_css_start, len);
}

static esp_err_t js_handler(httpd_req_t *req)
{
    size_t len = app_js_end - app_js_start;
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
    return httpd_resp_send(req, (const char *)app_js_start, len);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();

    /* Wi-Fi */
    wifi_ap_record_t ap;
    bool wifi_ok = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi, "connected", wifi_ok);
    if (wifi_ok) {
        cJSON_AddStringToObject(wifi, "ssid", (const char *)ap.ssid);
        cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
    }
    cJSON_AddItemToObject(r, "wifi", wifi);

    /* WebSocket / event bits */
    EventBits_t bits = xEventGroupGetBits(app_events);
    cJSON *ws = cJSON_CreateObject();
    cJSON_AddBoolToObject(ws, "connected", (bits & EVT_WS_CONNECTED) != 0);
    cJSON_AddItemToObject(r, "ws", ws);

    /* Flash / arm state */
    cJSON *flags = cJSON_CreateObject();
    cJSON_AddBoolToObject(flags, "flash_in_progress",
                          (bits & EVT_FLASH_IN_PROGRESS) != 0);
    cJSON_AddBoolToObject(flags, "bus_error",
                          (bits & EVT_BUS_ERROR) != 0);
    cJSON_AddBoolToObject(flags, "armed", arm_button_is_armed());
    cJSON_AddItemToObject(r, "flags", flags);

    /* System */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *model = "unknown";
    switch (chip.model) {
        case CHIP_ESP32:    model = "ESP32";    break;
        case CHIP_ESP32S2:  model = "ESP32-S2"; break;
        case CHIP_ESP32S3:  model = "ESP32-S3"; break;
        case CHIP_ESP32C3:  model = "ESP32-C3"; break;
        case CHIP_ESP32C6:  model = "ESP32-C6"; break;
        case CHIP_ESP32H2:  model = "ESP32-H2"; break;
        default: break;
    }

    uint32_t flash_size_bytes = 0;
    esp_flash_get_size(NULL, &flash_size_bytes);

    const esp_app_desc_t *app = esp_ota_get_app_description();

    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "mac", mac_str);
    cJSON_AddStringToObject(sys, "model", model);
    cJSON_AddNumberToObject(sys, "cores", chip.cores);
    cJSON_AddNumberToObject(sys, "flash_size", flash_size_bytes / (1024 * 1024));
    cJSON_AddNumberToObject(sys, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(sys, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(sys, "version", app ? app->version : "0.0.0");
    cJSON_AddItemToObject(r, "system", sys);

    return send_json(req, r);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* blocking */);
    if (err != ESP_OK) return send_error(req, 500, esp_err_to_name(err));

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;

    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    if (n > 0) {
        wifi_ap_record_t *records = calloc(n, sizeof(wifi_ap_record_t));
        if (!records) {
            cJSON_Delete(r);
            return send_error(req, 500, "alloc failed");
        }
        esp_wifi_scan_get_ap_records(&n, records);
        for (uint16_t i = 0; i < n; i++) {
            cJSON *net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "ssid", (const char *)records[i].ssid);
            cJSON_AddNumberToObject(net, "rssi", records[i].rssi);
            cJSON_AddNumberToObject(net, "channel", records[i].primary);
            cJSON_AddBoolToObject(net, "secured",
                                  records[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, net);
        }
        free(records);
    }
    cJSON_AddItemToObject(r, "networks", arr);
    return send_json(req, r);
}

static esp_err_t wifi_save_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_error(req, 400, "missing body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, 400, "invalid JSON");

    const cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *j_pwd  = cJSON_GetObjectItem(root, "pwd");

    if (!cJSON_IsString(j_ssid) || strlen(j_ssid->valuestring) == 0) {
        cJSON_Delete(root);
        return send_error(req, 400, "ssid required");
    }

    const char *ssid = j_ssid->valuestring;
    const char *pwd  = cJSON_IsString(j_pwd) ? j_pwd->valuestring : "";

    /* Salva in NVS */
    nvs_set_str_safe("wifi_ssid", ssid);
    nvs_set_str_safe("wifi_pwd",  pwd);

    /* Applica immediatamente */
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid,     ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pwd,  sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = strlen(pwd) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "message", "applying new credentials");
    cJSON_Delete(root);
    return send_json(req, r);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    char device_name[64], bitrate[16], tx_id[16], rx_id[16];
    nvs_get_str_default("dev_name",  device_name, sizeof(device_name), "Gateway-01");
    nvs_get_str_default("can_speed", bitrate,     sizeof(bitrate),     "500k");
    nvs_get_str_default("tx_id",     tx_id,       sizeof(tx_id),       "0x7E0");
    nvs_get_str_default("rx_id",     rx_id,       sizeof(rx_id),       "0x7E8");

    cJSON *r = cJSON_CreateObject();

    cJSON *can = cJSON_CreateObject();
    cJSON_AddStringToObject(can, "bitrate", bitrate);
    cJSON_AddStringToObject(can, "tx_id", tx_id);
    cJSON_AddStringToObject(can, "rx_id", rx_id);
    cJSON_AddNumberToObject(can, "tx_gpio", CONFIG_GW_CAN_TX_GPIO);
    cJSON_AddNumberToObject(can, "rx_gpio", CONFIG_GW_CAN_RX_GPIO);
    cJSON_AddItemToObject(r, "can", can);

    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "name", device_name);
    cJSON_AddItemToObject(r, "device", device);

    return send_json(req, r);
}

static esp_err_t config_save_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return send_error(req, 400, "missing body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, 400, "invalid JSON");

    /* CAN */
    const cJSON *can = cJSON_GetObjectItem(root, "can");
    if (cJSON_IsObject(can)) {
        const cJSON *br = cJSON_GetObjectItem(can, "bitrate");
        const cJSON *tx = cJSON_GetObjectItem(can, "tx_id");
        const cJSON *rx = cJSON_GetObjectItem(can, "rx_id");
        if (cJSON_IsString(br)) nvs_set_str_safe("can_speed", br->valuestring);
        if (cJSON_IsString(tx)) nvs_set_str_safe("tx_id", tx->valuestring);
        if (cJSON_IsString(rx)) nvs_set_str_safe("rx_id", rx->valuestring);
    }

    /* Device */
    const cJSON *device = cJSON_GetObjectItem(root, "device");
    if (cJSON_IsObject(device)) {
        const cJSON *name = cJSON_GetObjectItem(device, "name");
        if (cJSON_IsString(name)) {
            char tmp[64];
            strncpy(tmp, name->valuestring, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = 0;
            str_trim(tmp);
            if (tmp[0]) nvs_set_str_safe("dev_name", tmp);
        }
    }

    cJSON_Delete(root);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "message", "restart to apply CAN settings");
    return send_json(req, r);
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    send_json(req, r);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t factory_handler(httpd_req_t *req)
{
    /* Cancella solo il namespace del gateway: la calibrazione PHY Wi-Fi e
     * altri dati di sistema in altri namespace restano integri. */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "message", "wiped, rebooting");
    send_json(req, r);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* CORS preflight per sviluppo da browser su host diverso */
static esp_err_t cors_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* WebSocket endpoint /ws                                             */
/* ------------------------------------------------------------------ */
static void ws_set_client(httpd_handle_t hd, int fd)
{
    if (g_ws_mtx) xSemaphoreTake(g_ws_mtx, portMAX_DELAY);
    g_ws_server = hd;
    g_ws_fd     = fd;
    xEventGroupSetBits(app_events, EVT_WS_CONNECTED);
    if (g_ws_mtx) xSemaphoreGive(g_ws_mtx);
}

static void ws_clear_client(int fd)
{
    if (g_ws_mtx) xSemaphoreTake(g_ws_mtx, portMAX_DELAY);
    if (g_ws_fd == fd) {
        g_ws_fd     = -1;
        g_ws_server = NULL;
        xEventGroupClearBits(app_events, EVT_WS_CONNECTED);
    }
    if (g_ws_mtx) xSemaphoreGive(g_ws_mtx);
}

static void ws_close_callback(httpd_handle_t hd, int fd)
{
    ESP_LOGI(TAG, "WS client disconnected fd=%d", fd);
    ws_clear_client(fd);
    /* httpd vuole che chiudiamo il socket noi quando lru_purge_enable */
    close(fd);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake completato. Salvo il fd per i send asincroni. */
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS client connected fd=%d", fd);
        ws_set_client(req->handle, fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;

    /* Prima chiamata: leggo solo la lunghezza */
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws recv frame len failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_clear_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    if (frame.len == 0) return ESP_OK;
    if (frame.len > 16 * 1024) {
        ESP_LOGW(TAG, "ws frame too big: %u", (unsigned)frame.len);
        return ESP_FAIL;
    }

    frame.payload = malloc(frame.len + 1);
    if (!frame.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        frame.payload[frame.len] = 0;
        ws_proto_handle_json((const char *)frame.payload, frame.len);
    }
    free(frame.payload);
    return ret;
}

bool http_server_ws_connected(void)
{
    return g_ws_fd >= 0 && g_ws_server != NULL;
}

bool http_server_ws_send_text(const char *data, size_t len)
{
    if (!data) return false;
    httpd_handle_t hd;
    int fd;
    if (g_ws_mtx) xSemaphoreTake(g_ws_mtx, portMAX_DELAY);
    hd = g_ws_server;
    fd = g_ws_fd;
    if (g_ws_mtx) xSemaphoreGive(g_ws_mtx);
    if (!hd || fd < 0) return false;

    httpd_ws_frame_t f = {
        .final      = true,
        .fragmented = false,
        .type       = HTTPD_WS_TYPE_TEXT,
        .payload    = (uint8_t *)data,
        .len        = len,
    };
    esp_err_t r = httpd_ws_send_frame_async(hd, fd, &f);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "ws send failed (%s), dropping client",
                 esp_err_to_name(r));
        ws_clear_client(fd);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* URI table                                                          */
/* ------------------------------------------------------------------ */
static const httpd_uri_t uri_root  = { .uri = "/",                .method = HTTP_GET,  .handler = root_handler };
static const httpd_uri_t uri_css   = { .uri = "/app.css",         .method = HTTP_GET,  .handler = css_handler };
static const httpd_uri_t uri_js    = { .uri = "/app.js",          .method = HTTP_GET,  .handler = js_handler };
static const httpd_uri_t uri_stat  = { .uri = "/api/status",      .method = HTTP_GET,  .handler = status_handler };
static const httpd_uri_t uri_scan  = { .uri = "/api/scan",        .method = HTTP_GET,  .handler = scan_handler };
static const httpd_uri_t uri_wifi  = { .uri = "/api/wifi",        .method = HTTP_POST, .handler = wifi_save_handler };
static const httpd_uri_t uri_cget  = { .uri = "/api/config",      .method = HTTP_GET,  .handler = config_get_handler };
static const httpd_uri_t uri_cset  = { .uri = "/api/config",      .method = HTTP_POST, .handler = config_save_handler };
static const httpd_uri_t uri_rb    = { .uri = "/api/reboot",      .method = HTTP_POST, .handler = reboot_handler };
static const httpd_uri_t uri_fr    = { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_handler };
static const httpd_uri_t uri_opt   = { .uri = "/api/*",           .method = HTTP_OPTIONS, .handler = cors_options };
static const httpd_uri_t uri_ws    = {
    .uri          = "/ws",
    .method       = HTTP_GET,
    .handler      = ws_handler,
    .user_ctx     = NULL,
    .is_websocket = true,
};

void http_server_start(void)
{
    if (server) return;
    if (!g_ws_mtx) g_ws_mtx = xSemaphoreCreateMutex();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size        = 8192;
    cfg.max_uri_handlers  = 16;
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.send_wait_timeout = 15;
    cfg.recv_wait_timeout = 15;
    cfg.close_fn          = ws_close_callback;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "server start failed");
        return;
    }
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_css);
    httpd_register_uri_handler(server, &uri_js);
    httpd_register_uri_handler(server, &uri_stat);
    httpd_register_uri_handler(server, &uri_scan);
    httpd_register_uri_handler(server, &uri_wifi);
    httpd_register_uri_handler(server, &uri_cget);
    httpd_register_uri_handler(server, &uri_cset);
    httpd_register_uri_handler(server, &uri_rb);
    httpd_register_uri_handler(server, &uri_fr);
    httpd_register_uri_handler(server, &uri_opt);
    httpd_register_uri_handler(server, &uri_ws);

    ESP_LOGI(TAG, "HTTP server started on port 80 (REST + WebSocket /ws)");
}

void http_server_stop(void)
{
    if (server) { httpd_stop(server); server = NULL; }
}

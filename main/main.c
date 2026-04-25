/*
 * ESP32 UDS Gateway - Main
 *
 * Architettura:
 *   - twai_task:      gestisce RX/TX frame CAN sul driver TWAI
 *   - isotp_task:     ricompone/frammenta pacchetti ISO-TP (ISO 15765-2)
 *   - uds_task:       macchina a stati servizi UDS (ISO 14229)
 *   - ws_task:        client WebSocket verso backend, serializzazione JSON
 *   - supervisor:     watchdog applicativo, health, tester-present automatico
 *
 * Le comunicazioni tra task avvengono esclusivamente via FreeRTOS queue.
 * Nessun accesso condiviso diretto a buffer: serializzazione garantita.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "gw_nvs.h"
#include "twai_driver.h"
#include "isotp_layer.h"
#include "uds_service.h"
#include "ws_proto.h"
#include "tester_present.h"
#include "arm_button.h"
#include "status_led.h"
#include "http_server.h"

extern void seed_to_key_register(void);

static const char *TAG = "MAIN";

/* Queue globali (definite in app_config.h come extern) */
QueueHandle_t q_twai_rx;       /* twai_frame_t      -> isotp_task */
QueueHandle_t q_twai_tx;       /* twai_frame_t      <- isotp_task */
QueueHandle_t q_isotp_rx;      /* isotp_message_t   -> uds_task */
QueueHandle_t q_isotp_tx;      /* isotp_message_t   <- uds_task */
QueueHandle_t q_uds_request;   /* uds_request_t     -> uds_task (da ws) */
QueueHandle_t q_uds_response;  /* uds_response_t    <- uds_task (verso ws) */
QueueHandle_t q_can_monitor;   /* can_monitor_frame_t -> ws_proto_pump_task */

EventGroupHandle_t app_events;

static void wifi_init_sta(void);
static can_bitrate_t read_can_bitrate(void);

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 UDS Gateway booting...");

    /* NVS per credenziali Wi-Fi e config persistente */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Creazione queue */
    q_twai_rx      = xQueueCreate(32, sizeof(twai_frame_t));
    q_twai_tx      = xQueueCreate(32, sizeof(twai_frame_t));
    /*
     * Le code ISO-TP e UDS usano struct grandi (~4 KB l'una).
     * Profondità 2: sufficiente per il flusso sequenziale UDS e risparmia
     * ~99 KB di heap (critico con BSS aumentato dai buffer statici).
     */
    q_isotp_rx     = xQueueCreate(2,  sizeof(isotp_message_t));
    q_isotp_tx     = xQueueCreate(2,  sizeof(isotp_message_t));
    q_uds_request  = xQueueCreate(2,  sizeof(uds_request_t));
    q_uds_response = xQueueCreate(2,  sizeof(uds_response_t));
    /* Depth 16: CAN a 500 kbit/s può arrivare a ~3500 frame/s; il pump
     * task svuota ogni 20 ms, quindi 16 frame sono ampiamente sufficienti
     * per il traffico UDS tipico (pochi frame per ciclo request/response).
     * sizeof(can_monitor_frame_t) ~24 B → 384 B totali. */
    q_can_monitor  = xQueueCreate(16, sizeof(can_monitor_frame_t));

    configASSERT(q_twai_rx && q_twai_tx && q_isotp_rx && q_isotp_tx);
    configASSERT(q_uds_request && q_uds_response && q_can_monitor);

    app_events = xEventGroupCreate();
    configASSERT(app_events);

    /*
     * Inizializzazione moduli che hanno strutture interne (queue/mutex/sem).
     * DEVE avvenire PRIMA di creare i task che le usano e PRIMA di qualsiasi
     * chiamata API che dipende da loro (es. led_set_state).
     */
    led_init();
    isotp_init();
    tester_present_init();

    /* Stato LED iniziale (ora che la queue esiste) */
    led_set_state(LED_STATE_BOOT);

    /* Bring-up rete */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_sta();

    /* Bring-up driver TWAI (CAN): bitrate letto da NVS (fallback Kconfig) */
    ESP_ERROR_CHECK(twai_driver_init(read_can_bitrate()));

    /* Registro callback seed->key (personalizzabile per costruttore) */
    seed_to_key_register();

    /*
     * Creazione task con priorità decrescenti:
     * CAN e ISO-TP hanno priorità alta per rispettare i timing UDS (P2 ~50ms).
     * WebSocket ha priorità bassa: la rete non deve mai bloccare il bus.
     */
    xTaskCreatePinnedToCore(led_task,             "led",    4096, NULL,  3, NULL, 0);
    xTaskCreatePinnedToCore(twai_task,            "twai",   4096, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(isotp_task,           "isotp",  4096, NULL,  9, NULL, 1);
    xTaskCreatePinnedToCore(uds_task,             "uds",    8192, NULL,  8, NULL, 1);
    xTaskCreatePinnedToCore(tester_present_task,  "tp",     3072, NULL,  6, NULL, 1);
    xTaskCreatePinnedToCore(arm_button_task,      "arm",    2048, NULL,  4, NULL, 0);
    /* Pump locale: bridge q_uds_response -> WebSocket interno (http_server) */
    xTaskCreatePinnedToCore(ws_proto_pump_task,   "wspump", 4096, NULL,  5, NULL, 0);

    /* Stato iniziale: in attesa di Wi-Fi */
    led_set_state(LED_STATE_WIFI_CONNECTING);

    ESP_LOGI(TAG, "All tasks started");
}

/* ------------------------------------------------------------------ */
/* Wi-Fi station minimale: in produzione aggiungi provisioning sicuro */
/* ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
        led_set_state(LED_STATE_WIFI_CONNECTING);
        xEventGroupClearBits(app_events, EVT_WIFI_CONNECTED);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected");
        xEventGroupSetBits(app_events, EVT_WIFI_CONNECTED);
        /* Pronto: il browser apre /ws per usare la console. */
        led_set_state(LED_STATE_READY);
        http_server_start();
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_any, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &h_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &h_ip));

    /*
     * Credenziali da NVS (provisionate via UI http_server). Fallback ai
     * default di Kconfig: utili solo in sviluppo. In produzione entrambi i
     * Kconfig dovrebbero essere vuoti.
     */
    char ssid[33] = {0};
    char pwd[65]  = {0};
    gw_nvs_get_str("wifi_ssid", ssid, sizeof(ssid), CONFIG_GW_WIFI_SSID_DEFAULT);
    gw_nvs_get_str("wifi_pwd",  pwd,  sizeof(pwd),  CONFIG_GW_WIFI_PASSWORD_DEFAULT);

    if (ssid[0] == 0) {
        ESP_LOGW(TAG, "no Wi-Fi SSID configured (NVS empty, Kconfig default empty)");
    } else {
        ESP_LOGI(TAG, "Wi-Fi STA connecting to SSID: %s", ssid);
    }

    wifi_config_t wc = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wc.sta.ssid,     ssid, sizeof(wc.sta.ssid)     - 1);
    strncpy((char *)wc.sta.password, pwd,  sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (ssid[0] != 0) ESP_ERROR_CHECK(esp_wifi_connect());
}

/* ------------------------------------------------------------------ */
/* CAN bitrate: NVS "can_speed" (stringa 250k/500k/1m) -> enum         */
/* ------------------------------------------------------------------ */
static can_bitrate_t read_can_bitrate(void)
{
    const char *kconfig_default =
#if defined(CONFIG_GW_CAN_BITRATE_250K)
        "250k";
#elif defined(CONFIG_GW_CAN_BITRATE_1M)
        "1m";
#else
        "500k";
#endif
    char buf[8] = {0};
    gw_nvs_get_str("can_speed", buf, sizeof(buf), kconfig_default);

    if (strcmp(buf, "250k") == 0) return CAN_BITRATE_250K;
    if (strcmp(buf, "1m") == 0 || strcmp(buf, "1M") == 0) return CAN_BITRATE_1M;
    return CAN_BITRATE_500K;
}

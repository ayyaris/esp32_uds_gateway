/*
 * Tester Present automatico.
 *
 * Invia 0x3E 0x80 ogni TP_INTERVAL_MS sul canale configurato.
 * Il bit 0x80 (suppressPositiveResponse) evita che la ECU risponda,
 * riducendo il traffico sul bus.
 *
 * Si disabilita automaticamente durante una sequenza di flash e
 * quando la sessione ritorna default.
 */

#include "tester_present.h"
#include "isotp_layer.h"
#include "uds_service.h"
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "TP";

#define TP_INTERVAL_MS      2500

static volatile bool     tp_active       = false;
static volatile uint32_t tp_tx_id        = 0;
static volatile uint32_t tp_rx_id        = 0;
static volatile uint8_t  tp_session_type = UDS_SESSION_DEFAULT;
static SemaphoreHandle_t tp_lock = NULL;

void tester_present_init(void)
{
    if (!tp_lock) {
        tp_lock = xSemaphoreCreateMutex();
        configASSERT(tp_lock);
    }
}

void tester_present_start(uint32_t tx_id, uint32_t rx_id, uint8_t session_type)
{
    if (!tp_lock) return;   /* non ancora inizializzato */
    xSemaphoreTake(tp_lock, portMAX_DELAY);
    tp_tx_id        = tx_id;
    tp_rx_id        = rx_id;
    tp_session_type = session_type;
    tp_active       = (session_type != UDS_SESSION_DEFAULT);
    xSemaphoreGive(tp_lock);
    ESP_LOGI(TAG, "tester present %s (session=0x%02X)",
             tp_active ? "started" : "stopped", session_type);
}

void tester_present_stop(void)
{
    if (!tp_lock) return;
    xSemaphoreTake(tp_lock, portMAX_DELAY);
    tp_active = false;
    xSemaphoreGive(tp_lock);
    ESP_LOGI(TAG, "tester present stopped");
}

bool tester_present_is_active(void) { return tp_active; }

void tester_present_task(void *arg)
{
    if (!tp_lock) tester_present_init();  /* safety net */

    uint8_t payload[2] = { 0x3E, 0x80 };   /* SID 0x3E, sub 0x80 (suppress) */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TP_INTERVAL_MS));

        if (!tp_active) continue;

        /* Non inviare durante flash: quella sequenza produce già traffico */
        EventBits_t bits = xEventGroupGetBits(app_events);
        if (bits & EVT_FLASH_IN_PROGRESS) continue;
        if (!(bits & EVT_WIFI_CONNECTED)) {
            /* nessun impatto sul Wi-Fi, ma evita di spammare se WS offline
             * (comunque non serve internet per parlare al bus CAN) */
        }

        uint32_t tx, rx;
        xSemaphoreTake(tp_lock, portMAX_DELAY);
        tx = tp_tx_id;
        rx = tp_rx_id;
        xSemaphoreGive(tp_lock);

        /*
         * Invio diretto via isotp_send: SF da 2 byte (no attesa risposta).
         * Attenzione: se un'altra richiesta UDS è in corso, isotp_send
         * serializza tramite mutex interno, quindi potremmo attendere.
         * Il timeout è breve: se il bus è occupato, saltiamo.
         */
        if (!isotp_send(tx, rx, payload, sizeof(payload), 500)) {
            ESP_LOGW(TAG, "tester present send failed");
        }
    }
}

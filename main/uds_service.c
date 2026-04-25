/*
 * UDS service task + sequenza di flash completa.
 *
 * Architettura:
 *   - uds_task:       consuma uds_request_t da q_uds_request (web app)
 *   - uds_request_blocking(): usato internamente dalla sequenza di flash
 *   - uds_flash_sequence(): esegue tutto il ciclo download SENZA round-trip
 *                           con la web app. La webapp manda solo "start flash"
 *                           e riceve update di progresso.
 *
 * Response Pending (NRC 0x78): gestita in wait_response estendendo la
 * deadline a P2* (5s, extendibile).
 */

#include "uds_service.h"
#include "isotp_layer.h"
#include "tester_present.h"
#include "status_led.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "UDS";

static seed_to_key_fn_t  g_s2k_fn       = NULL;
static SemaphoreHandle_t g_uds_bus_lock = NULL;

void uds_set_seed_to_key(seed_to_key_fn_t fn) { g_s2k_fn = fn; }

/* ------------------------------------------------------------------ */
/* Attesa risposta UDS con gestione Response Pending                  */
/* ------------------------------------------------------------------ */
static bool wait_uds_response(uint32_t rx_id, uint8_t sid,
                              uint8_t *resp_payload, uint16_t *resp_len,
                              uint8_t *out_nrc,
                              uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    /* Static: isotp_message_t is ~4108 B; single-threaded UDS flow */
    static isotp_message_t in;

    while (xTaskGetTickCount() < deadline) {
        TickType_t remaining = deadline - xTaskGetTickCount();
        if (xQueueReceive(q_isotp_rx, &in, remaining) != pdTRUE) break;
        if (in.rx_id != rx_id || in.length < 2) continue;

        if (in.data[0] == UDS_NEGATIVE_RESPONSE && in.data[1] == sid) {
            uint8_t nrc = in.data[2];
            if (nrc == NRC_RESPONSE_PENDING) {
                /* estendi P2* (default 5s) */
                deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
                continue;
            }
            if (out_nrc) *out_nrc = nrc;
            if (resp_len) *resp_len = 0;
            return false;
        }
        if (in.data[0] == sid + UDS_POSITIVE_RESPONSE_OFFSET) {
            uint16_t n = in.length - 1;
            if (resp_payload && resp_len) {
                if (n > *resp_len) n = *resp_len;
                memcpy(resp_payload, &in.data[1], n);
                *resp_len = n;
            }
            if (out_nrc) *out_nrc = 0;
            return true;
        }
    }
    if (out_nrc) *out_nrc = 0xFF;  /* segnala timeout */
    if (resp_len) *resp_len = 0;
    return false;
}

/* ------------------------------------------------------------------ */
/* Richiesta UDS bloccante (API interna)                              */
/* ------------------------------------------------------------------ */
bool uds_request_blocking(uint32_t tx_id, uint32_t rx_id,
                          uint8_t sid,
                          const uint8_t *payload, uint16_t payload_len,
                          uint8_t *resp_payload, uint16_t *resp_len,
                          uint8_t *out_nrc,
                          uint32_t timeout_ms)
{
    /* Static: each ~4095–4108 B; single-threaded UDS flow assumed */
    static uint8_t buf[ISOTP_MAX_PAYLOAD];
    static isotp_message_t drain;
    if (payload_len + 1 > sizeof(buf)) return false;
    buf[0] = sid;
    if (payload_len) memcpy(&buf[1], payload, payload_len);

    /* svuota eventuali risposte residue */
    while (xQueueReceive(q_isotp_rx, &drain, 0) == pdTRUE) { /* drop */ }

    if (!isotp_send(tx_id, rx_id, buf, payload_len + 1, 5000)) {
        ESP_LOGW(TAG, "isotp_send failed sid=0x%02X", sid);
        if (out_nrc) *out_nrc = 0xFE;
        return false;
    }

    return wait_uds_response(rx_id, sid, resp_payload, resp_len,
                             out_nrc, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* Helper UDS                                                         */
/* ------------------------------------------------------------------ */
static bool uds_diagnostic_session(uint32_t tx_id, uint32_t rx_id,
                                   uint8_t session)
{
    uint8_t p[1] = { session };
    uint8_t resp[8];
    uint16_t rl = sizeof(resp);
    uint8_t nrc = 0;
    bool ok = uds_request_blocking(tx_id, rx_id,
                                   UDS_SID_DIAGNOSTIC_SESSION_CONTROL,
                                   p, 1, resp, &rl, &nrc, 2000);
    if (!ok) ESP_LOGW(TAG, "DSC fail nrc=0x%02X", nrc);
    return ok;
}

static bool uds_security_access(uint32_t tx_id, uint32_t rx_id, uint8_t level)
{
    if (!g_s2k_fn) {
        ESP_LOGE(TAG, "seed->key callback non impostata");
        return false;
    }

    /* Request seed: sub-function = level (dispari) */
    static uint8_t seed_buf[ISOTP_MAX_PAYLOAD]; /* 4095 B – static to avoid stack overflow */
    uint16_t seed_len = sizeof(seed_buf);
    uint8_t nrc = 0;
    uint8_t sub_seed[1] = { level };
    if (!uds_request_blocking(tx_id, rx_id, UDS_SID_SECURITY_ACCESS,
                              sub_seed, 1, seed_buf, &seed_len, &nrc, 2000)) {
        ESP_LOGW(TAG, "SecurityAccess seed fail nrc=0x%02X", nrc);
        return false;
    }
    /* seed_buf[0] == level (echo), seed reale dopo */
    if (seed_len < 2) return false;
    const uint8_t *seed = &seed_buf[1];
    uint16_t real_seed_len = seed_len - 1;

    uint8_t key_buf[64];
    uint16_t key_len = 0;
    if (!g_s2k_fn(level, seed, real_seed_len, key_buf, &key_len)) {
        ESP_LOGE(TAG, "seed->key callback refused");
        return false;
    }

    /* Send key: sub-function = level+1 (pari) */
    uint8_t sendkey[1 + sizeof(key_buf)];
    sendkey[0] = level + 1;
    memcpy(&sendkey[1], key_buf, key_len);

    uint8_t resp[8];
    uint16_t rl = sizeof(resp);
    bool ok = uds_request_blocking(tx_id, rx_id, UDS_SID_SECURITY_ACCESS,
                                   sendkey, 1 + key_len,
                                   resp, &rl, &nrc, 2000);
    if (!ok) ESP_LOGW(TAG, "SecurityAccess key fail nrc=0x%02X", nrc);
    return ok;
}

static bool uds_routine_control(uint32_t tx_id, uint32_t rx_id,
                                uint8_t sub, uint16_t routine_id,
                                const uint8_t *extra, uint16_t extra_len,
                                uint32_t timeout_ms)
{
    static uint8_t buf[ISOTP_MAX_PAYLOAD];  /* 4095 B – static to avoid stack overflow */
    static uint8_t resp[ISOTP_MAX_PAYLOAD]; /* 4095 B – static to avoid stack overflow */
    if (3 + extra_len > sizeof(buf)) return false;
    buf[0] = sub;
    buf[1] = (routine_id >> 8) & 0xFF;
    buf[2] = routine_id & 0xFF;
    if (extra_len) memcpy(&buf[3], extra, extra_len);

    uint16_t rl = sizeof(resp);
    uint8_t nrc = 0;
    bool ok = uds_request_blocking(tx_id, rx_id, UDS_SID_ROUTINE_CONTROL,
                                   buf, 3 + extra_len,
                                   resp, &rl, &nrc, timeout_ms);
    if (!ok) ESP_LOGW(TAG, "RoutineControl sub=0x%02X rid=0x%04X fail nrc=0x%02X",
                     sub, routine_id, nrc);
    return ok;
}

/* ------------------------------------------------------------------ */
/* RequestDownload                                                    */
/*   Ritorna il max_block_size concordato con la ECU (LengthFormatId) */
/* ------------------------------------------------------------------ */
static bool uds_request_download(uint32_t tx_id, uint32_t rx_id,
                                 uint32_t addr, uint32_t size,
                                 uint8_t addr_len, uint8_t size_len,
                                 uint16_t *max_block_size_out)
{
    if (addr_len < 1 || addr_len > 4 || size_len < 1 || size_len > 4) return false;
    uint8_t buf[16];
    uint8_t idx = 0;
    buf[idx++] = 0x00;                                   /* data format identifier: no compression/encryption */
    buf[idx++] = (size_len << 4) | addr_len;             /* address & length format identifier */
    for (int i = addr_len - 1; i >= 0; i--) buf[idx++] = (addr >> (i * 8)) & 0xFF;
    for (int i = size_len - 1; i >= 0; i--) buf[idx++] = (size >> (i * 8)) & 0xFF;

    uint8_t resp[16];
    uint16_t rl = sizeof(resp);
    uint8_t nrc = 0;
    if (!uds_request_blocking(tx_id, rx_id, UDS_SID_REQUEST_DOWNLOAD,
                              buf, idx, resp, &rl, &nrc, 5000)) {
        ESP_LOGW(TAG, "RequestDownload fail nrc=0x%02X", nrc);
        return false;
    }
    /* resp: [lengthFormatId(1)][maxNumberOfBlockLength(n)]
     * lengthFormatId high nibble = numero di byte del campo successivo
     */
    if (rl < 2) return false;
    uint8_t lfi_len = (resp[0] >> 4) & 0x0F;
    if (lfi_len == 0 || lfi_len > 4 || rl < 1 + lfi_len) return false;
    uint32_t mbs = 0;
    for (uint8_t i = 0; i < lfi_len; i++) mbs = (mbs << 8) | resp[1 + i];
    /* maxNumberOfBlockLength include il SID TransferData e il blockSequenceCounter */
    if (mbs < 3) return false;
    if (mbs > ISOTP_MAX_PAYLOAD) mbs = ISOTP_MAX_PAYLOAD;
    *max_block_size_out = (uint16_t)(mbs - 2);  /* tolgo SID+BSC */
    ESP_LOGI(TAG, "RequestDownload OK, chunk=%u", *max_block_size_out);
    return true;
}

static bool uds_transfer_data(uint32_t tx_id, uint32_t rx_id,
                              uint8_t bsc, const uint8_t *chunk, uint16_t chunk_len)
{
    static uint8_t buf[ISOTP_MAX_PAYLOAD]; /* 4095 B – static to avoid stack overflow */
    if (1 + chunk_len > sizeof(buf)) return false;
    buf[0] = bsc;
    memcpy(&buf[1], chunk, chunk_len);

    uint8_t resp[4];
    uint16_t rl = sizeof(resp);
    uint8_t nrc = 0;
    bool ok = uds_request_blocking(tx_id, rx_id, UDS_SID_TRANSFER_DATA,
                                   buf, 1 + chunk_len,
                                   resp, &rl, &nrc, 5000);
    if (!ok) ESP_LOGW(TAG, "TransferData bsc=0x%02X fail nrc=0x%02X", bsc, nrc);
    return ok;
}

static bool uds_request_transfer_exit(uint32_t tx_id, uint32_t rx_id)
{
    uint8_t resp[8];
    uint16_t rl = sizeof(resp);
    uint8_t nrc = 0;
    bool ok = uds_request_blocking(tx_id, rx_id, UDS_SID_REQUEST_TRANSFER_EXIT,
                                   NULL, 0, resp, &rl, &nrc, 5000);
    if (!ok) ESP_LOGW(TAG, "RequestTransferExit fail nrc=0x%02X", nrc);
    return ok;
}

static bool uds_ecu_reset(uint32_t tx_id, uint32_t rx_id, uint8_t reset_type)
{
    uint8_t p[1] = { reset_type };
    uint8_t resp[4];
    uint16_t rl = sizeof(resp);
    uint8_t nrc = 0;
    return uds_request_blocking(tx_id, rx_id, UDS_SID_ECU_RESET,
                                p, 1, resp, &rl, &nrc, 3000);
}

/* ------------------------------------------------------------------ */
/* Sequenza di flash completa                                         */
/* ------------------------------------------------------------------ */
flash_result_t uds_flash_sequence(const uds_flash_params_t *p)
{
    /*
     * Macro locale: rilascia sempre sia il flag che il mutex prima di tornare.
     * Usata su ogni path di errore in modo che nessun early-return lo dimentichi.
     */
#define FLASH_FAIL(err) do {                                        \
        xEventGroupClearBits(app_events, EVT_FLASH_IN_PROGRESS);    \
        xSemaphoreGive(g_uds_bus_lock);                             \
        return (err);                                               \
    } while (0)

    if (!p || !p->firmware || p->firmware_len == 0 ||
        p->firmware_len != p->memory_size) {
        return FLASH_ERR_PARAMS;
    }

    /* Impedisce a uds_task di inviare richieste durante tutta la sequenza */
    if (!g_uds_bus_lock ||
        xSemaphoreTake(g_uds_bus_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "UDS bus occupato, flash rifiutato");
        return FLASH_ERR_PARAMS;
    }

    xEventGroupSetBits(app_events, EVT_FLASH_IN_PROGRESS);
    ESP_LOGI(TAG, "=== FLASH START addr=0x%08lX size=%lu ===",
             (unsigned long)p->memory_address, (unsigned long)p->memory_size);

    if (p->progress_cb) p->progress_cb("session", 0, 0);
    if (!uds_diagnostic_session(p->tx_id, p->rx_id, UDS_SESSION_PROGRAMMING))
        FLASH_FAIL(FLASH_ERR_SESSION);

    if (p->progress_cb) p->progress_cb("security", 0, 0);
    led_set_state(LED_STATE_SECURITY_UNLOCK);
    if (!uds_security_access(p->tx_id, p->rx_id, p->security_level))
        FLASH_FAIL(FLASH_ERR_SECURITY);

    /*
     * Durante la sessione di programming, tester_present_task non invia
     * nulla (EVT_FLASH_IN_PROGRESS è settato). Salviamo comunque il
     * contesto TP così al termine del flash la gestione automatica riprende.
     */
    tester_present_start(p->tx_id, p->rx_id, UDS_SESSION_PROGRAMMING);

    if (p->erase_before) {
        if (p->progress_cb) p->progress_cb("erase", 0, 0);
        led_set_state(LED_STATE_ERASING);
        uint8_t extra[8];
        for (int i = 3; i >= 0; i--) extra[3 - i]     = (p->memory_address >> (i * 8)) & 0xFF;
        for (int i = 3; i >= 0; i--) extra[4 + 3 - i] = (p->memory_size    >> (i * 8)) & 0xFF;
        if (!uds_routine_control(p->tx_id, p->rx_id, UDS_ROUTINE_START,
                                 p->erase_routine_id, extra, 8, 30000))
            FLASH_FAIL(FLASH_ERR_ERASE);
    }

    if (p->progress_cb) p->progress_cb("request_download", 0, 0);
    uint16_t max_chunk = 0;
    if (!uds_request_download(p->tx_id, p->rx_id,
                              p->memory_address, p->memory_size,
                              p->address_len ? p->address_len : 4,
                              p->size_len    ? p->size_len    : 4,
                              &max_chunk))
        FLASH_FAIL(FLASH_ERR_REQUEST_DOWNLOAD);
    if (p->max_block_size && p->max_block_size < max_chunk)
        max_chunk = p->max_block_size;

    led_set_state(LED_STATE_FLASHING);
    uint32_t offset = 0;
    uint8_t  bsc    = 1;
    while (offset < p->firmware_len) {
        uint32_t remaining = p->firmware_len - offset;
        uint16_t clen      = remaining > max_chunk ? max_chunk : (uint16_t)remaining;
        if (!uds_transfer_data(p->tx_id, p->rx_id, bsc,
                               &p->firmware[offset], clen))
            FLASH_FAIL(FLASH_ERR_TRANSFER);
        offset += clen;
        bsc = (bsc % 0xFF) + 1;
        if (p->progress_cb) p->progress_cb("transfer", offset, p->firmware_len);
    }

    if (p->progress_cb) p->progress_cb("transfer_exit", p->firmware_len, p->firmware_len);
    if (!uds_request_transfer_exit(p->tx_id, p->rx_id))
        FLASH_FAIL(FLASH_ERR_EXIT);

    if (p->check_routine_id) {
        if (p->progress_cb) p->progress_cb("verify", 0, 0);
        if (!uds_routine_control(p->tx_id, p->rx_id, UDS_ROUTINE_START,
                                 p->check_routine_id, NULL, 0, 30000))
            FLASH_FAIL(FLASH_ERR_CHECK);
    }

    if (p->progress_cb) p->progress_cb("reset", 0, 0);
    if (!uds_ecu_reset(p->tx_id, p->rx_id, 0x01 /* hardReset */))
        FLASH_FAIL(FLASH_ERR_RESET);

    tester_present_stop();
    xEventGroupClearBits(app_events, EVT_FLASH_IN_PROGRESS);
    xSemaphoreGive(g_uds_bus_lock);
    if (p->progress_cb) p->progress_cb("done", p->firmware_len, p->firmware_len);
    ESP_LOGI(TAG, "=== FLASH COMPLETE ===");
    return FLASH_OK;

#undef FLASH_FAIL
}

/* ------------------------------------------------------------------ */
/* Task UDS: consuma richieste "semplici" dalla web app               */
/* ------------------------------------------------------------------ */
void uds_task(void *arg)
{
    g_uds_bus_lock = xSemaphoreCreateMutex();
    configASSERT(g_uds_bus_lock);

    /* Static: req ~4136 B + resp ~4132 B would overflow the 8192 B stack */
    static uds_request_t  req;
    static uds_response_t resp;
    static uint8_t resp_buf[ISOTP_MAX_PAYLOAD];

    while (1) {
        if (xQueueReceive(q_uds_request, &req, portMAX_DELAY) != pdTRUE) continue;

        ESP_LOGI(TAG, "UDS req sid=0x%02X tx_id=0x%03X len=%d",
                 req.sid, req.tx_id, req.payload_len);

        memset(&resp, 0, sizeof(resp));
        strncpy(resp.req_id, req.req_id, sizeof(resp.req_id));
        resp.sid = req.sid;

        uint16_t rl  = sizeof(resp_buf);
        uint8_t  nrc = 0;
        TickType_t t0 = xTaskGetTickCount();

        /* Serializza l'accesso al bus CAN con uds_flash_sequence */
        xSemaphoreTake(g_uds_bus_lock, portMAX_DELAY);
        bool ok = uds_request_blocking(req.tx_id, req.rx_id, req.sid,
                                       req.payload, req.payload_len,
                                       resp_buf, &rl, &nrc,
                                       req.timeout_ms ? req.timeout_ms : 2000);
        xSemaphoreGive(g_uds_bus_lock);

        resp.elapsed_ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;

        if (ok) {
            resp.status      = UDS_STATUS_POSITIVE;
            resp.sid         = req.sid + UDS_POSITIVE_RESPONSE_OFFSET;
            resp.payload_len = rl;
            if (rl > 0) memcpy(resp.payload, resp_buf, rl);

            /*
             * Auto-gestione Tester Present: se la richiesta appena completata
             * è DiagnosticSessionControl, aggiorno lo stato TP in base alla
             * sessione richiesta dal client (primo byte del payload).
             */
            if (req.sid == UDS_SID_DIAGNOSTIC_SESSION_CONTROL &&
                req.payload_len >= 1) {
                uint8_t new_session = req.payload[0];
                if (new_session == UDS_SESSION_DEFAULT) {
                    tester_present_stop();
                    led_set_state(LED_STATE_READY);
                } else {
                    tester_present_start(req.tx_id, req.rx_id, new_session);
                    led_set_state(LED_STATE_UDS_ACTIVE);
                }
            }
        } else if (nrc == 0xFF) {
            resp.status = UDS_STATUS_TIMEOUT;
        } else if (nrc == 0xFE) {
            resp.status = UDS_STATUS_BUS_ERROR;
        } else {
            resp.status = UDS_STATUS_NEGATIVE;
            resp.nrc    = nrc;
        }

        if (xQueueSend(q_uds_response, &resp, pdMS_TO_TICKS(100)) != pdTRUE) {
            /* ws_task non sta drenando le risposte: log e scarta. */
            ESP_LOGW(TAG, "q_uds_response full, dropped reply id=%s", resp.req_id);
        }
    }
}

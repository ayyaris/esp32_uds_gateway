/*
 * ISO-TP layer (ISO 15765-2) - RX + TX multi-frame.
 *
 * TX:
 *   - SF  (len <= 7)     -> invio immediato
 *   - FF  (len 8..4095)  -> First Frame + attesa FC + loop CF con STmin,
 *                           nuova FC ogni BS frame se BS != 0
 * RX:
 *   - SF                 -> consegna immediata
 *   - FF                 -> prepara buffer + invia FC CTS(BS=0, STmin=0)
 *   - CF                 -> accumula fino a expected_len
 *   - FC                 -> aggiorna stato TX (CTS / WAIT / OVFL)
 *
 * Timeout:
 *   - N_Bs (attesa FC dopo FF/blocco): 1000 ms -> abort
 *   - N_Cr (attesa CF):                1000 ms -> abort
 *   - max 8 FC WAIT consecutive -> abort
 *
 * Limitazioni:
 *   - un solo canale TX e un solo canale RX attivi per volta
 *   - normal addressing (no extended/mixed)
 *   - padding fisso 0xAA
 */

#include "isotp_layer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"    /* xTaskGetTickCount, TickType_t */
#include <string.h>

static const char *TAG = "ISOTP";

#define ISOTP_PAD_BYTE     0xAA
#define ISOTP_N_BS_MS      1000
#define ISOTP_N_CR_MS      1000
#define ISOTP_FC_WAIT_MAX  8

#define PCI_SF 0x0
#define PCI_FF 0x1
#define PCI_CF 0x2
#define PCI_FC 0x3

#define FC_CTS  0x0
#define FC_WAIT 0x1
#define FC_OVFL 0x2

/* ------------------------------------------------------------------ */
/* API pubblica                                                       */
/* ------------------------------------------------------------------ */
bool isotp_send(uint32_t tx_id, uint32_t rx_id_fc,
                const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* Stato RX                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    bool       active;
    uint32_t   rx_id;
    uint16_t   expected_len;
    uint16_t   received_len;
    uint8_t    next_sn;
    uint8_t    buffer[ISOTP_MAX_PAYLOAD];
    TickType_t last_rx;
} isotp_rx_state_t;

static isotp_rx_state_t rx_state;

/* ------------------------------------------------------------------ */
/* Stato TX                                                           */
/* ------------------------------------------------------------------ */
typedef enum {
    TX_IDLE,
    TX_WAIT_FC,
    TX_SENDING_CF,
    TX_DONE,
    TX_ERROR,
} tx_state_e;

typedef struct {
    tx_state_e state;
    uint32_t   tx_id;
    uint32_t   rx_id;
    uint8_t    buffer[ISOTP_MAX_PAYLOAD];
    uint16_t   length;
    uint16_t   offset;
    uint8_t    sn;
    uint8_t    bs;
    uint8_t    bs_counter;
    uint8_t    stmin_ms;
    TickType_t last_cf_tx;
    TickType_t wait_fc_start;
    uint8_t    wait_fc_count;
    SemaphoreHandle_t done_sem;
} isotp_tx_state_t;

static isotp_tx_state_t tx_state;
static SemaphoreHandle_t tx_mutex = NULL;

/* ------------------------------------------------------------------ */
/* Ultima coppia TX/RX usata per rispondere correttamente al FC       */
/* quando la richiesta era un SF e tx_state è già tornato IDLE.       */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t   tx_id;
    uint32_t   rx_id;
    TickType_t timestamp;
} last_tx_t;

static last_tx_t s_last_tx;
#define LAST_TX_VALID_MS 500

/* ------------------------------------------------------------------ */
/* Init (da chiamare prima di isotp_send e prima dello start del task) */
/* ------------------------------------------------------------------ */
void isotp_init(void)
{
    if (!tx_mutex) {
        tx_mutex = xSemaphoreCreateMutex();
        configASSERT(tx_mutex);
    }
    if (!tx_state.done_sem) {
        tx_state.done_sem = xSemaphoreCreateBinary();
        configASSERT(tx_state.done_sem);
    }
    memset(&s_last_tx, 0, sizeof(s_last_tx));
}

/* ------------------------------------------------------------------ */
/* Helper                                                             */
/* ------------------------------------------------------------------ */
static void pad_frame(twai_frame_t *f, uint8_t from)
{
    for (uint8_t i = from; i < 8; i++) f->data[i] = ISOTP_PAD_BYTE;
    f->dlc = 8;
}

static bool enqueue_frame(const twai_frame_t *f)
{
    return xQueueSend(q_twai_tx, f, pdMS_TO_TICKS(50)) == pdTRUE;
}

static uint8_t stmin_to_ms(uint8_t stmin)
{
    if (stmin <= 0x7F) return stmin;
    if (stmin >= 0xF1 && stmin <= 0xF9) return 1;  /* microsecondi -> 1 ms */
    return 1;
}

/* ------------------------------------------------------------------ */
/* Frame builders                                                     */
/* ------------------------------------------------------------------ */
static bool send_sf(uint32_t tx_id, const uint8_t *data, uint8_t len)
{
    twai_frame_t f = { .id = tx_id, .extended = false };
    f.data[0] = (PCI_SF << 4) | (len & 0x0F);
    memcpy(&f.data[1], data, len);
    pad_frame(&f, 1 + len);
    return enqueue_frame(&f);
}

static bool send_ff(uint32_t tx_id, const uint8_t *data, uint16_t total_len)
{
    twai_frame_t f = { .id = tx_id, .extended = false, .dlc = 8 };
    f.data[0] = (PCI_FF << 4) | ((total_len >> 8) & 0x0F);
    f.data[1] = total_len & 0xFF;
    memcpy(&f.data[2], data, 6);
    return enqueue_frame(&f);
}

static bool send_cf(uint32_t tx_id, uint8_t sn,
                    const uint8_t *data, uint8_t len)
{
    twai_frame_t f = { .id = tx_id, .extended = false };
    f.data[0] = (PCI_CF << 4) | (sn & 0x0F);
    memcpy(&f.data[1], data, len);
    pad_frame(&f, 1 + len);
    return enqueue_frame(&f);
}

static void send_fc_cts(uint32_t tx_id)
{
    twai_frame_t fc = { .id = tx_id, .extended = false };
    fc.data[0] = (PCI_FC << 4) | FC_CTS;
    fc.data[1] = 0x00;  /* BS=0 */
    fc.data[2] = 0x00;  /* STmin=0 */
    pad_frame(&fc, 3);
    enqueue_frame(&fc);
}

/* ------------------------------------------------------------------ */
/* API pubblica: invio messaggio completo                             */
/* ------------------------------------------------------------------ */
bool isotp_send(uint32_t tx_id, uint32_t rx_id_fc,
                const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    if (len == 0 || len > ISOTP_MAX_PAYLOAD) return false;
    if (!tx_mutex) return false;  /* init non ancora chiamato */

    xSemaphoreTake(tx_mutex, portMAX_DELAY);

    if (len <= 7) {
        bool ok = send_sf(tx_id, data, (uint8_t)len);
        if (ok) {
            s_last_tx.tx_id = tx_id;
            s_last_tx.rx_id = rx_id_fc;
            s_last_tx.timestamp = xTaskGetTickCount();
            ESP_LOGW(TAG, "SF sent tx=0x%03X rx_fc=0x%03X len=%u",
                     (unsigned)tx_id, (unsigned)rx_id_fc, (unsigned)len);
        } else {
            ESP_LOGW(TAG, "SF enqueue failed tx=0x%03X", (unsigned)tx_id);
        }
        xSemaphoreGive(tx_mutex);
        return ok;
    }

    /* svuota eventuale semaforo residuo */
    xSemaphoreTake(tx_state.done_sem, 0);

    tx_state.state         = TX_WAIT_FC;
    tx_state.tx_id         = tx_id;
    tx_state.rx_id         = rx_id_fc;
    memcpy(tx_state.buffer, data, len);
    tx_state.length        = len;
    tx_state.offset        = 6;
    tx_state.sn            = 1;
    tx_state.bs            = 0;
    tx_state.bs_counter    = 0;
    tx_state.stmin_ms      = 0;
    tx_state.wait_fc_count = 0;
    tx_state.wait_fc_start = xTaskGetTickCount();

    if (!send_ff(tx_id, data, len)) {
        tx_state.state = TX_IDLE;
        xSemaphoreGive(tx_mutex);
        return false;
    }

    bool ok = false;
    if (xSemaphoreTake(tx_state.done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        ok = (tx_state.state == TX_DONE);
    } else {
        ESP_LOGW(TAG, "isotp_send overall timeout len=%d", len);
    }
    tx_state.state = TX_IDLE;
    xSemaphoreGive(tx_mutex);
    return ok;
}

/* ------------------------------------------------------------------ */
/* FC handling (chiamato dal parser RX)                               */
/* ------------------------------------------------------------------ */
static void handle_fc(const twai_frame_t *f)
{
    if (tx_state.state != TX_WAIT_FC) return;
    if (f->id != tx_state.rx_id) return;

    uint8_t fs    = f->data[0] & 0x0F;
    uint8_t bs    = f->data[1];
    uint8_t stmin = f->data[2];

    switch (fs) {
        case FC_CTS:
            tx_state.bs         = bs;
            tx_state.bs_counter = 0;
            tx_state.stmin_ms   = stmin_to_ms(stmin);
            tx_state.last_cf_tx = 0;
            tx_state.state      = TX_SENDING_CF;
            break;
        case FC_WAIT:
            tx_state.wait_fc_count++;
            tx_state.wait_fc_start = xTaskGetTickCount();
            if (tx_state.wait_fc_count > ISOTP_FC_WAIT_MAX) {
                ESP_LOGW(TAG, "too many FC WAIT");
                tx_state.state = TX_ERROR;
                xSemaphoreGive(tx_state.done_sem);
            }
            break;
        default:
            ESP_LOGW(TAG, "FC overflow/unknown fs=%d", fs);
            tx_state.state = TX_ERROR;
            xSemaphoreGive(tx_state.done_sem);
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Pump TX (chiamata in ogni iterazione del task)                     */
/* ------------------------------------------------------------------ */
static void tx_pump(void)
{
    if (tx_state.state == TX_WAIT_FC) {
        if ((xTaskGetTickCount() - tx_state.wait_fc_start) >
             pdMS_TO_TICKS(ISOTP_N_BS_MS)) {
            ESP_LOGW(TAG, "N_Bs timeout");
            tx_state.state = TX_ERROR;
            xSemaphoreGive(tx_state.done_sem);
        }
        return;
    }

    if (tx_state.state != TX_SENDING_CF) return;

    if (tx_state.last_cf_tx != 0) {
        TickType_t elapsed = xTaskGetTickCount() - tx_state.last_cf_tx;
        if (elapsed < pdMS_TO_TICKS(tx_state.stmin_ms)) return;
    }

    uint16_t remaining = tx_state.length - tx_state.offset;
    uint8_t  chunk     = remaining > 7 ? 7 : (uint8_t)remaining;

    if (!send_cf(tx_state.tx_id, tx_state.sn,
                 &tx_state.buffer[tx_state.offset], chunk)) {
        return;  /* queue piena, ritento */
    }

    tx_state.offset     += chunk;
    tx_state.sn          = (tx_state.sn + 1) & 0x0F;
    tx_state.bs_counter++;
    tx_state.last_cf_tx  = xTaskGetTickCount();

    if (tx_state.offset >= tx_state.length) {
        tx_state.state = TX_DONE;
        xSemaphoreGive(tx_state.done_sem);
        return;
    }

    if (tx_state.bs != 0 && tx_state.bs_counter >= tx_state.bs) {
        tx_state.state         = TX_WAIT_FC;
        tx_state.wait_fc_start = xTaskGetTickCount();
    }
}

/* ------------------------------------------------------------------ */
/* RX                                                                 */
/* ------------------------------------------------------------------ */
static void deliver_rx(uint32_t rx_id, const uint8_t *buf, uint16_t len)
{
    /* Static: sizeof(isotp_message_t) ~4103 B, isotp_task stack is only 4096 B.
     * Safe because deliver_rx is only called from isotp_task (single caller)
     * and xQueueSend copies the struct into the queue before returning. */
    static isotp_message_t msg;
    if (len > ISOTP_MAX_PAYLOAD) len = ISOTP_MAX_PAYLOAD;
    msg.rx_id  = rx_id;
    msg.tx_id  = 0;
    msg.length = len;
    memcpy(msg.data, buf, len);
    if (xQueueSend(q_isotp_rx, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "q_isotp_rx full, dropped msg rx_id=0x%03X len=%u",(unsigned)rx_id, (unsigned)len);
    }
}

static void handle_rx_frame(const twai_frame_t *f)
{
    if (f->dlc < 1) return;
    uint8_t pci = (f->data[0] & 0xF0) >> 4;

    switch (pci) {
        case PCI_SF: {
            uint8_t len = f->data[0] & 0x0F;
            if (len == 0 || len > 7) return;
            if (f->dlc < (uint8_t)(len + 1)) return;
            deliver_rx(f->id, &f->data[1], len);
            break;
        }
        case PCI_FF: {
            if (f->dlc < 8) return;
            uint16_t dlen = ((uint16_t)(f->data[0] & 0x0F) << 8) | f->data[1];
            if (dlen < 8 || dlen > ISOTP_MAX_PAYLOAD) {
                ESP_LOGW(TAG, "FF: invalid DL=%u, ignored", (unsigned)dlen);
                return;
            }
            rx_state.active       = true;
            rx_state.rx_id        = f->id;
            rx_state.expected_len = dlen;
            memcpy(rx_state.buffer, &f->data[2], 6);
            rx_state.received_len = 6;
            rx_state.next_sn      = 1;
            rx_state.last_rx      = xTaskGetTickCount();

            /*
             * FC address: se c'è una richiesta TX attiva e il FF proviene
             * dall'ID di risposta atteso, usiamo il tx_id della richiesta
             * (es. 0x7E0 se stiamo parlando con 0x7E8). Questo funziona per
             * qualunque coppia di ID, non solo la convenzione OBD-II ±8.
             * Fallback a f->id - 8 solo per frame non sollecitati.
             */
            uint32_t fc_addr;
            if (tx_state.state != TX_IDLE && f->id == tx_state.rx_id) {
                fc_addr = tx_state.tx_id;
            } else if (s_last_tx.timestamp &&
                       (xTaskGetTickCount() - s_last_tx.timestamp) < pdMS_TO_TICKS(LAST_TX_VALID_MS) &&
                       f->id == s_last_tx.rx_id) {
                fc_addr = s_last_tx.tx_id;
            } else {
                fc_addr = (f->id >= 8) ? (f->id - 8) : 0;   /* OBD-II fallback */
            }
            ESP_LOGI(TAG, "FF rx_id=0x%03X -> FC tx_id=0x%03X", (unsigned)f->id, (unsigned)fc_addr);
            send_fc_cts(fc_addr);
            break;
        }
        case PCI_CF: {
            if (!rx_state.active) return;
            uint8_t sn = f->data[0] & 0x0F;
            if (sn != rx_state.next_sn) {
                ESP_LOGW(TAG, "CF SN mismatch exp=%d got=%d",
                         rx_state.next_sn, sn);
                rx_state.active = false;
                return;
            }
            /* Guard: should not happen if FF was validated, but be safe. */
            if (rx_state.received_len >= rx_state.expected_len) {
                rx_state.active = false;
                return;
            }
            uint16_t rem     = rx_state.expected_len - rx_state.received_len;
            uint16_t to_copy = rem > 7 ? 7 : rem;
            if (f->dlc < (uint8_t)(to_copy + 1)) return;
            memcpy(&rx_state.buffer[rx_state.received_len],
                   &f->data[1], to_copy);
            rx_state.received_len += to_copy;
            rx_state.next_sn      = (rx_state.next_sn + 1) & 0x0F;
            rx_state.last_rx      = xTaskGetTickCount();

            if (rx_state.received_len >= rx_state.expected_len) {
                deliver_rx(rx_state.rx_id, rx_state.buffer,
                           rx_state.expected_len);
                rx_state.active = false;
            }
            break;
        }
        case PCI_FC:
            handle_fc(f);
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Task principale                                                    */
/* ------------------------------------------------------------------ */
void isotp_task(void *arg)
{
    twai_frame_t rx_frame;

    /* rx_state va azzerato, tx_state no (potrebbe avere done_sem già settato). */
    memset(&rx_state, 0, sizeof(rx_state));

    /* Safety net: se isotp_init non è stato chiamato prima, lo chiamiamo ora.
     * NB: meglio che il main chiami isotp_init() prima di questo task. */
    if (!tx_mutex || !tx_state.done_sem) isotp_init();

    while (1) {
        /* RX dal CAN */
        if (xQueueReceive(q_twai_rx, &rx_frame, pdMS_TO_TICKS(2)) == pdTRUE) {
            handle_rx_frame(&rx_frame);
        }

        /* Pump TX multi-frame in corso */
        tx_pump();

        /* Timeout N_Cr su RX multi-frame */
        if (rx_state.active &&
            (xTaskGetTickCount() - rx_state.last_rx) >
             pdMS_TO_TICKS(ISOTP_N_CR_MS)) {
            ESP_LOGW(TAG, "N_Cr timeout");
            rx_state.active = false;
        }
    }
}

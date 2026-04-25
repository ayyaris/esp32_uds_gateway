/*
 * WS2812 status LED driver.
 *
 * Usa il nuovo driver RMT di ESP-IDF 5.x (driver/rmt_tx.h) che supporta
 * encoder personalizzati. Il protocollo WS2812 richiede:
 *   - T0H: 0.35 us high, T0L: 0.80 us low  -> bit 0
 *   - T1H: 0.70 us high, T1L: 0.60 us low  -> bit 1
 *   - Reset: >= 50 us low
 * Tolleranza ~±150 ns. A 10 MHz di resolution_hz, ogni tick = 100 ns.
 *
 * Palette stati:
 *   BOOT             spento
 *   WIFI_CONNECTING  ciano respirante
 *   WS_CONNECTING    ciano respirante più veloce
 *   NO_BACKEND       arancione respirante lento
 *   AP_MODE          arancione pulsante
 *   READY            verde soft fisso
 *   ARMED            giallo pulsante rapido
 *   UDS_ACTIVE       blu fisso tenue
 *   FLASHING         viola pulsante (heartbeat)
 *   ERASING          viola-rosa rapido
 *   SECURITY_UNLOCK  magenta lento
 *   BUS_OFF          rosso rapido
 *   FLASH_ERROR      rosso fisso
 *   FAULT_HARDWARE   rosso SOS-like
 */

#include "status_led.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

static const char *TAG = "LED";

#define RMT_RES_HZ        10000000   /* 10 MHz -> 100 ns per tick */
#define WS2812_T0H_TICKS  3          /* 300 ns */
#define WS2812_T0L_TICKS  9          /* 900 ns */
#define WS2812_T1H_TICKS  7          /* 700 ns */
#define WS2812_T1L_TICKS  6          /* 600 ns */
#define WS2812_RESET_US   50

/* ------------------------------------------------------------------ */
/* RMT encoder per WS2812                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    rmt_symbol_word_t reset_code;
    int state;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder,
                            rmt_channel_handle_t channel,
                            const void *primary_data, size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_handle_t bytes_enc = enc->bytes_encoder;
    rmt_encoder_handle_t copy_enc  = enc->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded = 0;

    switch (enc->state) {
        case 0: {
            encoded += bytes_enc->encode(bytes_enc, channel,
                                          primary_data, data_size,
                                          &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) enc->state = 1;
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
        } /* fallthrough */
        case 1: {
            encoded += copy_enc->encode(copy_enc, channel,
                                         &enc->reset_code,
                                         sizeof(enc->reset_code),
                                         &session_state);
            if (session_state & RMT_ENCODING_COMPLETE) {
                enc->state = 0;
                state |= RMT_ENCODING_COMPLETE;
            }
            if (session_state & RMT_ENCODING_MEM_FULL) {
                state |= RMT_ENCODING_MEM_FULL;
                goto out;
            }
        }
    }
out:
    *ret_state = state;
    return encoded;
}

static esp_err_t ws2812_encoder_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

static esp_err_t ws2812_encoder_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = 0;
    return ESP_OK;
}

static esp_err_t ws2812_encoder_new(rmt_encoder_handle_t *ret)
{
    ws2812_encoder_t *enc = calloc(1, sizeof(*enc));
    if (!enc) return ESP_ERR_NO_MEM;

    enc->base.encode = ws2812_encode;
    enc->base.del    = ws2812_encoder_del;
    enc->base.reset  = ws2812_encoder_reset;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = { .level0 = 1, .duration0 = WS2812_T0H_TICKS,
                  .level1 = 0, .duration1 = WS2812_T0L_TICKS },
        .bit1 = { .level0 = 1, .duration0 = WS2812_T1H_TICKS,
                  .level1 = 0, .duration1 = WS2812_T1L_TICKS },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder));

    rmt_copy_encoder_config_t copy_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder));

    uint32_t reset_ticks = RMT_RES_HZ / 1000000 * WS2812_RESET_US / 2;
    enc->reset_code = (rmt_symbol_word_t) {
        .level0 = 0, .duration0 = reset_ticks,
        .level1 = 0, .duration1 = reset_ticks,
    };

    *ret = &enc->base;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Stato task                                                          */
/* ------------------------------------------------------------------ */
static rmt_channel_handle_t  rmt_chan = NULL;
static rmt_encoder_handle_t  encoder  = NULL;

static volatile led_state_t g_current_state = LED_STATE_BOOT;
static volatile led_state_t g_override      = LED_STATE_MAX;
static volatile int64_t     g_override_until_us = 0;
static volatile int64_t     g_activity_until_us = 0;
static volatile uint8_t     g_activity_color[3] = {0, 0, 0};

/* Coda comandi: usata solo per serializzare le chiamate set_state */
static QueueHandle_t q_led_cmd;

typedef struct {
    led_state_t state;
} led_cmd_t;

/* ------------------------------------------------------------------ */
/* Output WS2812: il LED usa ordinamento GRB                           */
/* ------------------------------------------------------------------ */
static void led_write_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    /* applica brightness */
    r = (uint16_t)r * LED_BRIGHTNESS / 255;
    g = (uint16_t)g * LED_BRIGHTNESS / 255;
    b = (uint16_t)b * LED_BRIGHTNESS / 255;

    uint8_t frame[3] = { g, r, b };  /* GRB order */
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(rmt_chan, encoder, frame, sizeof(frame), &tx_cfg);
    rmt_tx_wait_all_done(rmt_chan, pdMS_TO_TICKS(1000));
}

/* ------------------------------------------------------------------ */
/* Helpers animazione                                                  */
/* ------------------------------------------------------------------ */

/* Sinusoide 0..1 con periodo T ms, phase in ms */
static float breathe(int64_t t_ms, int period_ms)
{
    float phase = (float)(t_ms % period_ms) / period_ms;
    float s = sinf(phase * 2.0f * (float)M_PI);
    return (s + 1.0f) * 0.5f;
}

/* Square wave 0 o 1 con duty cycle */
static int pulse(int64_t t_ms, int period_ms, int duty_pct)
{
    int phase = (int)(t_ms % period_ms);
    return phase < (period_ms * duty_pct / 100) ? 1 : 0;
}


/* ------------------------------------------------------------------ */
/* Render di uno stato -> RGB corrente per tempo t                     */
/* ------------------------------------------------------------------ */
static void render_state(led_state_t s, int64_t t_ms,
                         uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = *g = *b = 0;

    switch (s) {
        case LED_STATE_OFF:
        case LED_STATE_BOOT:
            break;

        /* --- Connessione in corso --- */
        case LED_STATE_WIFI_CONNECTING: {
            /* ciano respirante lento - 2 s */
            float k = breathe(t_ms, 2000);
            *r = 0;   *g = (uint8_t)(180 * k);  *b = (uint8_t)(255 * k);
            break;
        }
        case LED_STATE_WS_CONNECTING: {
            /* ciano respirante più veloce - 1 s */
            float k = breathe(t_ms, 1000);
            *r = 0;   *g = (uint8_t)(200 * k);  *b = (uint8_t)(255 * k);
            break;
        }

        /* --- Ready --- */
        case LED_STATE_READY:
            /* verde soft fisso */
            *r = 0; *g = 120; *b = 20;
            break;

        /* --- Warning --- */
        case LED_STATE_NO_BACKEND: {
            /* arancione respirante */
            float k = 0.4f + 0.6f * breathe(t_ms, 1500);
            *r = (uint8_t)(255 * k);  *g = (uint8_t)(90 * k);  *b = 0;
            break;
        }
        case LED_STATE_AP_MODE: {
            /* arancione pulsante quadrato (invita a connettersi) */
            int on = pulse(t_ms, 800, 50);
            *r = on ? 255 : 60;  *g = on ? 140 : 30;  *b = 0;
            break;
        }

        /* --- Operativo --- */
        case LED_STATE_UDS_ACTIVE:
            /* blu fisso tenue */
            *r = 0; *g = 50; *b = 200;
            break;

        case LED_STATE_ARMED: {
            /* giallo pulsante rapido - attenzione: pronto a flashare */
            int on = pulse(t_ms, 500, 50);
            if (on) { *r = 255; *g = 200; *b = 0; }
            else    { *r = 60;  *g = 40;  *b = 0; }
            break;
        }

        case LED_STATE_SECURITY_UNLOCK: {
            /* magenta respirante */
            float k = 0.3f + 0.7f * breathe(t_ms, 1200);
            *r = (uint8_t)(200 * k);  *g = 0;  *b = (uint8_t)(180 * k);
            break;
        }

        case LED_STATE_FLASHING: {
            /* heartbeat viola: due battiti rapidi + pausa */
            int phase = (int)(t_ms % 1200);
            float k = 0;
            if      (phase < 100) k = phase / 100.0f;
            else if (phase < 200) k = 1.0f - (phase - 100) / 100.0f;
            else if (phase < 300) k = (phase - 200) / 100.0f;
            else if (phase < 400) k = 1.0f - (phase - 300) / 100.0f;
            *r = (uint8_t)(180 * k);  *g = 0;  *b = (uint8_t)(255 * k);
            break;
        }

        case LED_STATE_ERASING: {
            /* rosa/magenta rapido */
            float k = breathe(t_ms, 400);
            *r = (uint8_t)(255 * k);  *g = (uint8_t)(50 * k);  *b = (uint8_t)(150 * k);
            break;
        }

        /* --- Errori --- */
        case LED_STATE_BUS_OFF: {
            /* rosso rapido - problema bus */
            int on = pulse(t_ms, 200, 50);
            *r = on ? 255 : 30;  *g = 0;  *b = 0;
            break;
        }

        case LED_STATE_FLASH_ERROR:
            /* rosso fisso - ultimo flash ha fallito */
            *r = 200; *g = 0; *b = 0;
            break;

        case LED_STATE_FAULT_HARDWARE: {
            /* SOS-like: ... --- ... */
            int phase = (int)(t_ms % 3000);
            int on = 0;
            /* 3 short */
            if (phase < 150) on = 1;
            else if (phase < 300) on = 0;
            else if (phase < 450) on = 1;
            else if (phase < 600) on = 0;
            else if (phase < 750) on = 1;
            else if (phase < 1000) on = 0;
            /* 3 long */
            else if (phase < 1400) on = 1;
            else if (phase < 1550) on = 0;
            else if (phase < 1950) on = 1;
            else if (phase < 2100) on = 0;
            else if (phase < 2500) on = 1;
            *r = on ? 255 : 0;  *g = 0;  *b = 0;
            break;
        }

        case LED_STATE_CAN_TRAFFIC:
        case LED_STATE_MAX:
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* API pubblica                                                        */
/* ------------------------------------------------------------------ */
void led_init(void)
{
    if (!q_led_cmd) {
        q_led_cmd = xQueueCreate(8, sizeof(led_cmd_t));
        configASSERT(q_led_cmd);
    }
}

void led_set_state(led_state_t state)
{
    if (!q_led_cmd) return;
    led_cmd_t cmd = { .state = state };
    xQueueSend(q_led_cmd, &cmd, 0);
}

void led_flash_activity(bool is_tx)
{
    /* Non sovrascrivere stati prioritari (errori, flash in corso) */
    led_state_t s = g_current_state;
    if (s <= LED_STATE_ERASING) return;   /* flash/erase/errori prevalgono */

    int64_t now = esp_timer_get_time();
    g_activity_until_us = now + 60000;     /* 60 ms */
    if (is_tx) {
        g_activity_color[0] = 0;   g_activity_color[1] = 80;  g_activity_color[2] = 255;
    } else {
        g_activity_color[0] = 180; g_activity_color[1] = 0;   g_activity_color[2] = 255;
    }
}

void led_flash_error(void)
{
    g_override = LED_STATE_BUS_OFF;
    g_override_until_us = esp_timer_get_time() + 600000;  /* 600 ms */
}

/* ------------------------------------------------------------------ */
/* Task                                                                */
/* ------------------------------------------------------------------ */
static void rmt_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .clk_src          = RMT_CLK_SRC_DEFAULT,
        .gpio_num         = LED_GPIO,
        .mem_block_symbols= 64,
        .resolution_hz    = RMT_RES_HZ,
        .trans_queue_depth= 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &rmt_chan));
    ESP_ERROR_CHECK(ws2812_encoder_new(&encoder));
    ESP_ERROR_CHECK(rmt_enable(rmt_chan));
    ESP_LOGI(TAG, "WS2812 on GPIO%d initialized", LED_GPIO);
}

void led_task(void *arg)
{
    /* queue creata da led_init() prima dello start del task */
    if (!q_led_cmd) led_init();  /* safety net */

    rmt_init();
    led_write_rgb(0, 0, 0);   /* spegni all'avvio */

    TickType_t tick_period = pdMS_TO_TICKS(100);  /* ~33 fps, fluido per breathe */
    int64_t start_us = esp_timer_get_time();

    while (1) {
        led_cmd_t cmd;
        /* consuma tutti i comandi pending */
        while (xQueueReceive(q_led_cmd, &cmd, 0) == pdTRUE) {
            if (cmd.state != g_current_state) {
                g_current_state = cmd.state;
                start_us = esp_timer_get_time();   /* reset fase animazione */
            }
        }

        int64_t now_us = esp_timer_get_time();
        int64_t t_ms   = (now_us - start_us) / 1000;

        uint8_t r = 0, g = 0, b = 0;

        /* 1. Override attivo (es. led_flash_error) */
        if (g_override < LED_STATE_MAX && now_us < g_override_until_us) {
            render_state(g_override, t_ms, &r, &g, &b);
        } else {
            g_override = LED_STATE_MAX;
            /* 2. Stato di base */
            render_state(g_current_state, t_ms, &r, &g, &b);

            /* 3. Blink di attività CAN sovrapposto (schiarita verso il colore) */
            if (now_us < g_activity_until_us) {
                /* mix 50% con il colore di attività */
                r = (uint8_t)((r / 2) + (g_activity_color[0] / 2));
                g = (uint8_t)((g / 2) + (g_activity_color[1] / 2));
                b = (uint8_t)((b / 2) + (g_activity_color[2] / 2));
            }
        }

        led_write_rgb(r, g, b);
        vTaskDelay(tick_period);
    }
}

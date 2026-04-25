/*
 * TWAI (CAN) driver wrapper.
 *
 * GPIO configurabili via "idf.py menuconfig" -> "ESP32 UDS Gateway"
 * -> "CAN transceiver wiring". Default TX=27, RX=26 (ESP32-DevKitC).
 * Transceiver esterno obbligatorio (SN65HVD230, TJA1050, ecc.).
 */

#include "twai_driver.h"
#include "status_led.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "TWAI";

#define TWAI_TX_GPIO   ((gpio_num_t)CONFIG_GW_CAN_TX_GPIO)
#define TWAI_RX_GPIO   ((gpio_num_t)CONFIG_GW_CAN_RX_GPIO)

esp_err_t twai_driver_init(can_bitrate_t bitrate)
{
    twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_GPIO, TWAI_RX_GPIO, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = 32;
    g_config.tx_queue_len = 32;

    twai_timing_config_t t_config;
    switch (bitrate) {
        case CAN_BITRATE_250K: t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS(); break;
        case CAN_BITRATE_1M:   t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();   break;
        case CAN_BITRATE_500K:
        default:               t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS(); break;
    }

    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "install failed: %s", esp_err_to_name(err)); return err; }

    err = twai_start();
    if (err != ESP_OK) { ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));  return err; }

    ESP_LOGI(TAG, "TWAI started @ %d kbit/s", bitrate == CAN_BITRATE_500K ? 500 :
                                               bitrate == CAN_BITRATE_250K ? 250 : 1000);
    return ESP_OK;
}

/*
 * Task TWAI:
 *  - preleva frame da q_twai_tx e li trasmette
 *  - riceve frame dal bus e li pusha in q_twai_rx
 *  - monitora errori bus e alerts
 */
void twai_task(void *arg)
{
    twai_frame_t tx_frame;
    twai_message_t msg;

    /* Abilita alerts utili per diagnostica del bus */
    uint32_t alerts = TWAI_ALERT_BUS_OFF | TWAI_ALERT_ERR_PASS |
                      TWAI_ALERT_TX_FAILED | TWAI_ALERT_RX_QUEUE_FULL;
    twai_reconfigure_alerts(alerts, NULL);

    while (1) {
        /* TX non bloccante: se c'è un frame da mandare lo mando subito */
        if (xQueueReceive(q_twai_tx, &tx_frame, 0) == pdTRUE) {
            twai_message_t out = {
                .identifier       = tx_frame.id,
                .extd             = tx_frame.extended ? 1 : 0,
                .data_length_code = tx_frame.dlc,
            };
            memcpy(out.data, tx_frame.data, tx_frame.dlc);
            esp_err_t e = twai_transmit(&out, pdMS_TO_TICKS(50));
            if (e != ESP_OK) {
                ESP_LOGW(TAG, "tx failed id=0x%03X: %s", tx_frame.id, esp_err_to_name(e));
                xEventGroupSetBits(app_events, EVT_BUS_ERROR);
                led_flash_error();
            } else {
                led_flash_activity(true);
                if (q_can_monitor) {
                    can_monitor_frame_t mf = {
                        .id    = tx_frame.id,
                        .dlc   = tx_frame.dlc,
                        .is_tx = true,
                        .ts_us = tx_frame.ts_us,
                    };
                    memcpy(mf.data, tx_frame.data, tx_frame.dlc);
                    xQueueSend(q_can_monitor, &mf, 0);
                }
            }
        }

        /* RX con piccolo timeout per yield */
        if (twai_receive(&msg, pdMS_TO_TICKS(5)) == ESP_OK) {
            twai_frame_t rx_frame = {
                .id       = msg.identifier,
                .extended = msg.extd,
                .dlc      = msg.data_length_code,
                .ts_us    = esp_timer_get_time(),
            };
            memcpy(rx_frame.data, msg.data, msg.data_length_code);
            if (xQueueSend(q_twai_rx, &rx_frame, 0) != pdTRUE) {
                ESP_LOGW(TAG, "q_twai_rx full, dropped id=0x%03X", rx_frame.id);
            }
            if (q_can_monitor) {
                can_monitor_frame_t mf = {
                    .id    = rx_frame.id,
                    .dlc   = rx_frame.dlc,
                    .is_tx = false,
                    .ts_us = rx_frame.ts_us,
                };
                memcpy(mf.data, rx_frame.data, rx_frame.dlc);
                xQueueSend(q_can_monitor, &mf, 0);
            }
            led_flash_activity(false);
        }

        /* Check alerts */
        uint32_t triggered = 0;
        twai_read_alerts(&triggered, 0);
        if (triggered & TWAI_ALERT_BUS_OFF) {
            ESP_LOGE(TAG, "BUS OFF, initiating recovery");
            twai_initiate_recovery();
            xEventGroupSetBits(app_events, EVT_BUS_ERROR);
            led_set_state(LED_STATE_BUS_OFF);
        }
    }
}

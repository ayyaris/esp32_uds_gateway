/*
 * Arm button: protezione hardware per operazioni di flash.
 *
 * Il flash di una ECU è irreversibile e può brickare la centralina.
 * Richiedere la pressione di un pulsante fisico sull'ESP32 prima di
 * autorizzarlo impedisce che un compromesso del backend cloud o delle
 * credenziali WebSocket basti a flashare un veicolo.
 *
 * Stati interni:
 *   DISARMED        -> normale
 *   ARMED           -> pulsante premuto, timer in corso
 *   ARMED_EXTENDED  -> pressione lunga, resta armato finché non consumato
 */

#include "arm_button.h"
#include "status_led.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"    /* vTaskDelay, xTaskGetTickCount */

static const char *TAG = "ARM";

#define DEBOUNCE_MS          50
#define LONG_PRESS_MS        3000

static portMUX_TYPE g_arm_spinlock = portMUX_INITIALIZER_UNLOCKED;

typedef enum {
    ARM_DISARMED,
    ARM_TIMED,
    ARM_EXTENDED,
} arm_state_t;

static volatile arm_state_t g_state    = ARM_DISARMED;
static volatile TickType_t  g_armed_at = 0;

bool arm_button_is_armed(void)
{
    if (g_state == ARM_DISARMED) return false;

    /* timeout automatico in stato ARM_TIMED */
    if (g_state == ARM_TIMED) {
        taskENTER_CRITICAL(&g_arm_spinlock);
        TickType_t armed_at = g_armed_at;
        taskEXIT_CRITICAL(&g_arm_spinlock);
        TickType_t elapsed_ms =
            (xTaskGetTickCount() - armed_at) * portTICK_PERIOD_MS;
        if (elapsed_ms > ARM_TIMEOUT_MS) {
            g_state = ARM_DISARMED;
            ESP_LOGW(TAG, "arm timeout, disarmed");
            return false;
        }
    }
    return true;
}

void arm_button_consume(void)
{
    g_state = ARM_DISARMED;
    ESP_LOGI(TAG, "arm consumed");
    /* non cambio lo stato LED qui: chi chiama consume() (es. flash runner)
     * sa meglio cosa mostrare dopo (READY / FLASH_ERROR) */
}

void arm_button_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << ARM_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    bool       last_level   = true;    /* idle alto (pull-up) */
    TickType_t press_start  = 0;
    bool       press_tracked = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        bool level = gpio_get_level(ARM_BUTTON_GPIO) != 0;

        if (level == last_level) {
            /* se tenuto premuto abbastanza: estendi */
            if (!level && press_tracked &&
                (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS > LONG_PRESS_MS &&
                g_state != ARM_EXTENDED) {
                g_state = ARM_EXTENDED;
                ESP_LOGW(TAG, "arm EXTENDED (long press)");
            }
            continue;
        }
        last_level = level;

        if (!level) {
            /* pressione: arma (timed) */
            press_start   = xTaskGetTickCount();
            press_tracked = true;
            taskENTER_CRITICAL(&g_arm_spinlock);
            g_armed_at = press_start;
            g_state    = ARM_TIMED;
            taskEXIT_CRITICAL(&g_arm_spinlock);
            ESP_LOGI(TAG, "ARMED for %d s", ARM_TIMEOUT_MS / 1000);
            led_set_state(LED_STATE_ARMED);
        } else {
            press_tracked = false;
        }
    }
}

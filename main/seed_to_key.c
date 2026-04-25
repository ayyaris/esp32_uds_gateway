/*
 * Seed-to-key.
 *
 * L'algoritmo qui sotto è quello usato dall'ECU SIMULATOR di test
 * (esp32_ecu_simulator): XOR con 0xA5A5A5A5 seguito da rotate-left di 7 bit.
 * Serve per poter testare end-to-end SecurityAccess contro il simulatore.
 *
 * !!! NON FUNZIONA SU ECU REALI !!!
 *
 * Ogni costruttore usa un algoritmo proprietario (VAG, Ford, Stellantis,
 * Toyota, ecc.). Devi ottenere l'algoritmo corretto per via legittima
 * (licenza OEM, SDK ufficiale, pass-thru J2534 dealer tool) e sostituire
 * la funzione my_seed_to_key() qui sotto.
 *
 * ECU moderne con HSM (Hardware Security Module) firmano il firmware
 * con chiavi asimmetriche non recuperabili; per quelle non è possibile
 * implementare seed-to-key senza la cooperazione del costruttore.
 *
 * In produzione disabilita CONFIG_GW_SEED_TO_KEY_DEMO via menuconfig:
 * seed_to_key_register() diventa un no-op e UDS 0x27 fallisce con
 * "seed->key callback non impostata" finché non registri il tuo algoritmo.
 */

#include "sdkconfig.h"
#include "uds_service.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "S2K";

#if CONFIG_GW_SEED_TO_KEY_DEMO

static uint32_t rotate_left_32(uint32_t x, uint8_t n)
{
    return (x << n) | (x >> (32 - n));
}

static bool my_seed_to_key(uint8_t level,
                           const uint8_t *seed, uint16_t seed_len,
                           uint8_t *key,  uint16_t *key_len_out)
{
    if (seed_len != 4) {
        ESP_LOGW(TAG, "expected 4-byte seed, got %u", seed_len);
        return false;
    }
    uint32_t s = ((uint32_t)seed[0] << 24) | ((uint32_t)seed[1] << 16) |
                 ((uint32_t)seed[2] << 8)  |  (uint32_t)seed[3];
    uint32_t k = rotate_left_32(s ^ 0xA5A5A5A5, 7);

    key[0] = (k >> 24) & 0xFF;
    key[1] = (k >> 16) & 0xFF;
    key[2] = (k >> 8)  & 0xFF;
    key[3] =  k        & 0xFF;
    *key_len_out = 4;

    ESP_LOGI(TAG, "level=0x%02X seed=0x%08lX key=0x%08lX",
             level, (unsigned long)s, (unsigned long)k);
    return true;
}

void seed_to_key_register(void)
{
    ESP_LOGW(TAG, "demo seed-to-key registered (TEST ONLY, incompatible with real ECUs)");
    uds_set_seed_to_key(my_seed_to_key);
}

#else  /* !CONFIG_GW_SEED_TO_KEY_DEMO */

void seed_to_key_register(void)
{
    ESP_LOGI(TAG, "demo seed-to-key disabled; register your OEM algorithm "
                  "with uds_set_seed_to_key() before using SecurityAccess");
}

#endif /* CONFIG_GW_SEED_TO_KEY_DEMO */

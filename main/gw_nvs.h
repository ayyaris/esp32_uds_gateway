#pragma once

/*
 * Accesso centralizzato alle stringhe di configurazione persistenti.
 * Tutte le chiavi risiedono nel namespace NVS "gw_cfg" (condiviso con
 * http_server.c, che fornisce la UI di provisioning).
 *
 * I Kconfig GW_*_DEFAULT sono fallback applicati solo quando la chiave
 * NVS non esiste ancora (primo boot / dopo factory reset).
 */

#include <stddef.h>
#include "esp_err.h"

/*
 * Legge una stringa dal namespace gw_cfg. Se la chiave non esiste o NVS
 * non è accessibile, copia `def` in `out` e ritorna ESP_OK. `out` è
 * sempre null-terminato. Ritorna un esp_err_t solo se anche il fallback
 * fallisce (out_sz == 0).
 */
esp_err_t gw_nvs_get_str(const char *key, char *out, size_t out_sz,
                         const char *def);

/*
 * Scrive una stringa nel namespace gw_cfg (commit incluso).
 */
esp_err_t gw_nvs_set_str(const char *key, const char *val);

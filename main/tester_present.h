#pragma once
#include "app_config.h"

/*
 * Tester Present automatico.
 *
 * Quando una sessione UDS non-default è attiva, la ECU chiude la
 * sessione dopo ~5 secondi di inattività (S3 timer). Per mantenerla
 * aperta si invia periodicamente il servizio 0x3E con sub-function
 * 0x80 (suppress positive response) ogni ~2-3 secondi.
 *
 * Uso:
 *   tester_present_start(tx_id, rx_id, UDS_SESSION_EXTENDED);
 *   ... lavoro diagnostico ...
 *   tester_present_stop();
 *
 * Il modulo NON invia Tester Present durante flash (EVT_FLASH_IN_PROGRESS),
 * perché la sequenza di flash ha già il suo traffico UDS continuo.
 */

void tester_present_task(void *arg);
/* Crea lock interno. Chiamare prima di start/stop o start del task. */
void tester_present_init(void);
void tester_present_start(uint32_t tx_id, uint32_t rx_id, uint8_t session_type);
void tester_present_stop(void);
bool tester_present_is_active(void);

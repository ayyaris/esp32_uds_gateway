#pragma once
#include "app_config.h"

void isotp_task(void *arg);

/* Crea mutex e semaforo interni. Chiamare prima di isotp_send(). */
void isotp_init(void);

/*
 * Invio bloccante di un messaggio ISO-TP completo.
 *   tx_id      : CAN ID su cui trasmettere
 *   rx_id_fc   : CAN ID atteso per la Flow Control (di ritorno)
 *   data/len   : payload (1..4095 byte)
 *   timeout_ms : timeout complessivo per completare la trasmissione
 * Ritorna true se il messaggio è stato completamente trasmesso.
 * Thread-safe: serializza chiamate concorrenti tramite mutex interno.
 */
bool isotp_send(uint32_t tx_id, uint32_t rx_id_fc,
                const uint8_t *data, uint16_t len, uint32_t timeout_ms);

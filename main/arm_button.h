#pragma once
#include "app_config.h"

/*
 * Arm button: GPIO fisico che deve essere attivo per autorizzare
 * operazioni distruttive (flash write). Senza questo, un attaccante
 * con accesso alla rete/backend potrebbe avviare un flash da remoto.
 *
 * Logica:
 *   - GPIO configurato come input con pull-up interno
 *   - Attivo a livello BASSO (pulsante verso GND)
 *   - Debounce software 50 ms
 *   - Quando premuto imposta EVT_FLASH_ARMED
 *   - Il bit viene resettato automaticamente dopo ARM_TIMEOUT_MS
 *     o al completamento di un flash
 *   - Long press (3 secondi) = arm esteso senza timeout (uso bench)
 */

#define ARM_BUTTON_GPIO   GPIO_NUM_0   /* BOOT button su devkit classici */
#define ARM_TIMEOUT_MS    30000        /* 30 s per avviare il flash dopo arm */

void arm_button_task(void *arg);
bool arm_button_is_armed(void);
void arm_button_consume(void);         /* chiamata dopo il flash per resettare */

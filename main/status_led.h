#pragma once
#include "app_config.h"

/*
 * WS2812 status LED driver.
 *
 * Un singolo LED RGB su GPIO4 che comunica lo stato del gateway
 * tramite colore + pattern di animazione.
 *
 * Architettura: task dedicato con priorità bassa, riceve comandi da
 * una coda FreeRTOS. Altri moduli chiamano led_set_state(...) che
 * posta nella coda - l'animazione è gestita nel task senza bloccare
 * i chiamanti.
 *
 * Usa il driver RMT di ESP-IDF per generare la sequenza WS2812
 * (800 kHz, protocollo NRZ con timing specifici).
 */

#define LED_GPIO         GPIO_NUM_4
#define LED_BRIGHTNESS   40      /* 0-255, 40 evita di accecare in penombra */

/*
 * Stati del sistema. L'elenco è in ordine di priorità: se due stati sono
 * attivi, vince quello con priorità più alta (indice più basso).
 */
typedef enum {
    /* Errori gravi - rosso */
    LED_STATE_FAULT_HARDWARE = 0,  /* TWAI install fallito, init error */
    LED_STATE_BUS_OFF,             /* CAN bus-off, recovery in corso */
    LED_STATE_FLASH_ERROR,         /* ultimo flash fallito */

    /* Operazioni critiche - viola/magenta pulsante */
    LED_STATE_FLASHING,            /* transfer in corso */
    LED_STATE_ERASING,             /* erase memory ECU */
    LED_STATE_SECURITY_UNLOCK,     /* seed/key exchange */

    /* Attività diagnostica - blu */
    LED_STATE_UDS_ACTIVE,          /* sessione UDS non-default aperta */
    LED_STATE_CAN_TRAFFIC,         /* blink corto su RX/TX */

    /* Armed - giallo pulsante */
    LED_STATE_ARMED,               /* pulsante di sicurezza premuto */

    /* Warning - arancione */
    LED_STATE_NO_BACKEND,          /* WS disconnesso ma Wi-Fi OK */
    LED_STATE_AP_MODE,             /* modalità provisioning */

    /* Connessione - ciano */
    LED_STATE_WIFI_CONNECTING,     /* in attesa Wi-Fi */
    LED_STATE_WS_CONNECTING,       /* in attesa backend */

    /* Idle pronto - verde soft */
    LED_STATE_READY,               /* tutto connesso, bus attivo, idle */

    /* Spento */
    LED_STATE_BOOT,                /* durante init */
    LED_STATE_OFF,

    LED_STATE_MAX
} led_state_t;

/* Inizializza le strutture interne (queue). Va chiamata PRIMA di qualsiasi
 * led_set_state() e prima di startare il task. */
void led_init(void);

/* Setta lo stato corrente. Chiamata thread-safe. */
void led_set_state(led_state_t state);

/* Flash corto (~60ms) indicativo di traffico CAN - usato per lampeggiare
 * al RX/TX di un frame UDS senza interferire con lo stato principale.
 * Se uno stato di priorità più alta è attivo (flash, error), viene ignorato. */
void led_flash_activity(bool is_tx);

/* Notifica un errore temporaneo (blink rosso 2x poi torna allo stato). */
void led_flash_error(void);

/* Task principale, chiamato da app_main. */
void led_task(void *arg);

#pragma once
#include "app_config.h"

/* Service ID UDS (ISO 14229-1) */
#define UDS_SID_DIAGNOSTIC_SESSION_CONTROL   0x10
#define UDS_SID_ECU_RESET                    0x11
#define UDS_SID_SECURITY_ACCESS              0x27
#define UDS_SID_COMMUNICATION_CONTROL        0x28
#define UDS_SID_TESTER_PRESENT               0x3E
#define UDS_SID_READ_DATA_BY_IDENTIFIER      0x22
#define UDS_SID_WRITE_DATA_BY_IDENTIFIER     0x2E
#define UDS_SID_READ_DTC_INFORMATION         0x19
#define UDS_SID_CLEAR_DTC                    0x14
#define UDS_SID_ROUTINE_CONTROL              0x31
#define UDS_SID_REQUEST_DOWNLOAD             0x34
#define UDS_SID_TRANSFER_DATA                0x36
#define UDS_SID_REQUEST_TRANSFER_EXIT        0x37

/* Session types */
#define UDS_SESSION_DEFAULT        0x01
#define UDS_SESSION_PROGRAMMING    0x02
#define UDS_SESSION_EXTENDED       0x03

/* Routine sub-functions */
#define UDS_ROUTINE_START          0x01
#define UDS_ROUTINE_STOP           0x02
#define UDS_ROUTINE_RESULTS        0x03

/* Response */
#define UDS_NEGATIVE_RESPONSE                0x7F
#define UDS_POSITIVE_RESPONSE_OFFSET         0x40

/* Negative Response Codes selezionati */
#define NRC_GENERAL_REJECT                   0x10
#define NRC_SERVICE_NOT_SUPPORTED            0x11
#define NRC_SUB_FUNCTION_NOT_SUPPORTED       0x12
#define NRC_CONDITIONS_NOT_CORRECT           0x22
#define NRC_REQUEST_SEQUENCE_ERROR           0x24
#define NRC_SECURITY_ACCESS_DENIED           0x33
#define NRC_INVALID_KEY                      0x35
#define NRC_EXCEEDED_NUMBER_OF_ATTEMPTS      0x36
#define NRC_RESPONSE_PENDING                 0x78

void uds_task(void *arg);

/*
 * Esegue una richiesta UDS bloccante da codice interno (es. flash task).
 * Ritorna true se risposta positiva.
 */
bool uds_request_blocking(uint32_t tx_id, uint32_t rx_id,
                          uint8_t sid,
                          const uint8_t *payload, uint16_t payload_len,
                          uint8_t *resp_payload, uint16_t *resp_len,
                          uint8_t *out_nrc,
                          uint32_t timeout_ms);

/*
 * Callback invocata dalla sequenza di flash per l'algoritmo seed->key
 * (proprietario del costruttore). Se non settata, SecurityAccess fallisce.
 *   level: livello di security (0x01, 0x03, ...)
 *   seed/seed_len: seed ricevuto dalla ECU
 *   key/key_len_out: buffer dove scrivere la key calcolata
 * Ritorna true se ha calcolato una key valida.
 */
typedef bool (*seed_to_key_fn_t)(uint8_t level,
                                 const uint8_t *seed, uint16_t seed_len,
                                 uint8_t *key, uint16_t *key_len_out);
void uds_set_seed_to_key(seed_to_key_fn_t fn);

/*
 * Callback di progress durante flash.
 *   phase: stringa descrittiva ("erase", "transfer", "verify", ...)
 *   done, total: byte completati / totali (o 0/0 per phase-only)
 */
typedef void (*flash_progress_fn_t)(const char *phase,
                                    uint32_t done, uint32_t total);

/*
 * Parametri della sequenza di flash.
 */
typedef struct {
    uint32_t tx_id;
    uint32_t rx_id;
    uint8_t  security_level;    /* es. 0x01 */
    uint32_t memory_address;    /* indirizzo di destinazione nella ECU */
    uint32_t memory_size;       /* dimensione del blob */
    uint8_t  address_len;       /* quanti byte per memory_address (tip. 4) */
    uint8_t  size_len;          /* quanti byte per memory_size    (tip. 4) */
    uint16_t max_block_size;    /* se 0, usa quello suggerito dalla ECU */
    bool     erase_before;      /* esegui RoutineControl eraseMemory */
    uint32_t erase_routine_id;  /* es. 0xFF00 */
    uint32_t check_routine_id;  /* es. 0xFF01 (checkProgrammingDependencies) */
    const uint8_t *firmware;
    uint32_t firmware_len;
    flash_progress_fn_t progress_cb;
} uds_flash_params_t;

typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_SESSION,
    FLASH_ERR_SECURITY,
    FLASH_ERR_ERASE,
    FLASH_ERR_REQUEST_DOWNLOAD,
    FLASH_ERR_TRANSFER,
    FLASH_ERR_EXIT,
    FLASH_ERR_CHECK,
    FLASH_ERR_RESET,
    FLASH_ERR_PARAMS,
} flash_result_t;

flash_result_t uds_flash_sequence(const uds_flash_params_t *p);

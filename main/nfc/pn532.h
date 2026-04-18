#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 基于 SAK 的卡型分类; Ultralight/NTAG21x 共用 SAK=0x00
typedef enum {
    PN532_CARD_UNKNOWN = 0,
    PN532_CARD_MIFARE_CLASSIC_1K,
    PN532_CARD_MIFARE_CLASSIC_4K,
    PN532_CARD_MIFARE_ULTRALIGHT,
} pn532_card_type_t;

typedef struct {
    uint8_t           uid[10];
    uint8_t           uid_len;
    uint16_t          atqa;
    uint8_t           sak;
    pn532_card_type_t type;
} pn532_target_t;

esp_err_t pn532_init(void);

// 成功返回 32 位版本号（ic<<24|ver<<16|..），失败返回 0
uint32_t pn532_get_firmware_version(void);

// InListPassiveTarget, timeout 内未发现卡返回 ESP_ERR_TIMEOUT
esp_err_t pn532_read_passive_target(pn532_target_t* out, uint32_t timeout_ms);

// InDataExchange; 成功时 rx 只含应用数据,不含 PN532 status byte
esp_err_t pn532_in_data_exchange(const uint8_t* tx, uint8_t tx_len,
                                 uint8_t* rx, uint8_t* rx_len,
                                 uint32_t timeout_ms);

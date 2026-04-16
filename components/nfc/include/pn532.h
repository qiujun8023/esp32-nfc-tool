#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 卡类型（基于 SAK）
typedef enum {
    PN532_CARD_UNKNOWN = 0,
    PN532_CARD_MIFARE_CLASSIC_1K,
    PN532_CARD_MIFARE_CLASSIC_4K,
    PN532_CARD_MIFARE_ULTRALIGHT,  // 含 NTAG21x
} pn532_card_type_t;

typedef struct {
    uint8_t           uid[10];
    uint8_t           uid_len;
    uint16_t          atqa;
    uint8_t           sak;
    pn532_card_type_t type;
} pn532_target_t;

esp_err_t pn532_init(void);

// 读取固件版本（验证通信），返回 32 位版本号；失败返回 0
uint32_t pn532_get_firmware_version(void);

// 等待 ISO14443A 卡进入视野，timeout_ms 内未发现返回 ESP_ERR_TIMEOUT
esp_err_t pn532_read_passive_target(pn532_target_t* out, uint32_t timeout_ms);

// 通用 InDataExchange，用于 Mifare auth/read/write
// 返回 PN532 响应数据（不含 status byte），rsp_len 是 in/out
esp_err_t pn532_in_data_exchange(const uint8_t* tx, uint8_t tx_len,
                                 uint8_t* rx, uint8_t* rx_len,
                                 uint32_t timeout_ms);

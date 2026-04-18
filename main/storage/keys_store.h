#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "mifare_classic.h"

// NVS blob 数量上限; 超出会拒绝新增
#define KEYS_USER_MAX 64

esp_err_t keys_store_init(void);

size_t keys_store_load_user(uint8_t out[][MFC_KEY_LEN], size_t max_n);

// 已存在则静默返回 ESP_OK (不算错误)
esp_err_t keys_store_add(const uint8_t key[MFC_KEY_LEN]);

esp_err_t keys_store_remove(size_t index);

// user 在前、default 在后,命中率高的放前面可提前结束爆破
size_t keys_store_combined(uint8_t out[][MFC_KEY_LEN], size_t max_n);

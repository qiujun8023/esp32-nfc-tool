#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "mifare_classic.h"

#define KEYS_USER_MAX 64

esp_err_t keys_store_init(void);

// 加载用户自定义 keys，返回数量
size_t keys_store_load_user(uint8_t out[][MFC_KEY_LEN], size_t max_n);

// 追加一条用户 key（去重）
esp_err_t keys_store_add(const uint8_t key[MFC_KEY_LEN]);

// 删除（按索引）
esp_err_t keys_store_remove(size_t index);

// 合并 user + default 到一个数组（user 在前，便于命中率）
// 返回总数
size_t keys_store_combined(uint8_t out[][MFC_KEY_LEN], size_t max_n);

#pragma once

#include <stddef.h>
#include <stdint.h>

// Mifare Classic 常见默认密钥（来自 cypher-pn532 + 公开 dictionary）
// 每条 6 字节，定义在 default_keys.c
#define MIFARE_DEFAULT_KEY_COUNT 50

extern const uint8_t MIFARE_DEFAULT_KEYS[MIFARE_DEFAULT_KEY_COUNT][6];

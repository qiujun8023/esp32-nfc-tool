#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pn532.h"

#define MFC_KEY_LEN 6
#define MFC_BLOCK_LEN 16
#define MFC_1K_SECTORS 16
#define MFC_4K_SECTORS 40
#define MFC_MAX_SECTORS MFC_4K_SECTORS
#define MFC_1K_BLOCKS 64
#define MFC_4K_BLOCKS 256
#define MFC_MAX_BLOCKS MFC_4K_BLOCKS

typedef enum {
    MFC_KEY_A = 0,
    MFC_KEY_B = 1,
} mfc_key_type_t;

typedef struct {
    bool    found;
    uint8_t key[MFC_KEY_LEN];
} mfc_known_key_t;

typedef struct {
    pn532_target_t  target;
    uint8_t         sector_count;
    uint16_t        block_count;
    mfc_known_key_t key_a[MFC_MAX_SECTORS];
    mfc_known_key_t key_b[MFC_MAX_SECTORS];
    uint8_t         data[MFC_MAX_BLOCKS][MFC_BLOCK_LEN];
    bool            block_read[MFC_MAX_BLOCKS];
} mfc_dump_t;

typedef void (*mfc_progress_cb)(uint8_t sector, uint8_t total, bool key_a_found, bool key_b_found, void* user);

esp_err_t mfc_authenticate(const uint8_t* uid, uint8_t uid_len, uint8_t block, mfc_key_type_t kt, const uint8_t* key);
esp_err_t mfc_read_block(uint8_t block, uint8_t out[MFC_BLOCK_LEN]);
esp_err_t mfc_write_block(uint8_t block, const uint8_t in[MFC_BLOCK_LEN]);

// keys 排列: 用户 key 优先,默认字典紧随,提高命中率
esp_err_t mfc_full_read(const pn532_target_t* target,
                        const uint8_t (*keys)[MFC_KEY_LEN], size_t keys_count,
                        mfc_dump_t* dump,
                        mfc_progress_cb cb, void* user);

// 返回前经过回读验证,verify_fail_count 写入不匹配的 block 数
esp_err_t mfc_full_write(const mfc_dump_t* dump,
                         mfc_progress_cb cb, void* user,
                         uint8_t* verify_fail_count);

typedef enum {
    MFC_MAGIC_NONE = 0,
    MFC_MAGIC_GEN1A,  // CUID/UFUID,通过后门认证写 block 0
    MFC_MAGIC_GEN2,   // FUID/CUID,用普通 key 认证后直接写 block 0
} mfc_magic_type_t;

mfc_magic_type_t mfc_detect_magic(void);

// MFC_MAGIC_NONE 时返回 NULL,便于调用方直接判空决定是否上报字段
const char* mfc_magic_str(mfc_magic_type_t t);

uint8_t mfc_sector_first_block(uint8_t sector);
uint8_t mfc_sector_block_count(uint8_t sector);
uint8_t mfc_sector_count_for_type(pn532_card_type_t type);

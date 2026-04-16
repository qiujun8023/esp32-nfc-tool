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
    uint8_t         block_count;  // 1K=64, 4K=256
    mfc_known_key_t key_a[MFC_MAX_SECTORS];
    mfc_known_key_t key_b[MFC_MAX_SECTORS];
    uint8_t         data[256][MFC_BLOCK_LEN];  // 块数据，足以容纳 4K
    bool            block_read[256];
} mfc_dump_t;

// 进度回调（字典攻击 / 完整 dump 中调用）
typedef void (*mfc_progress_cb)(uint8_t sector, uint8_t total, bool key_a_found, bool key_b_found, void* user);

// 单步操作
esp_err_t mfc_authenticate(const uint8_t* uid, uint8_t uid_len, uint8_t block, mfc_key_type_t kt, const uint8_t* key);
esp_err_t mfc_read_block(uint8_t block, uint8_t out[MFC_BLOCK_LEN]);
esp_err_t mfc_write_block(uint8_t block, const uint8_t in[MFC_BLOCK_LEN]);

// UID 克隆（写到 CUID/UFUID 魔术卡的 block 0），verify_ok 非空时回写验证结果
esp_err_t mfc_clone_uid(const uint8_t* new_uid, uint8_t uid_len, bool* verify_ok);

// 字典攻击 + 完整读卡，结果填入 dump
// keys：候选 key 数组（前 N 条用户的 + 默认 50 条）；keys_count 总数
esp_err_t mfc_full_read(const pn532_target_t* target,
                        const uint8_t (*keys)[MFC_KEY_LEN], size_t keys_count,
                        mfc_dump_t* dump,
                        mfc_progress_cb cb, void* user);

// 把 dump 写回卡（要求 key 已知），返回验证失败的 block 数
esp_err_t mfc_full_write(const mfc_dump_t* dump,
                         mfc_progress_cb cb, void* user,
                         uint8_t* verify_fail_count);

// Magic 卡类型
typedef enum {
    MFC_MAGIC_NONE = 0,  // 普通卡
    MFC_MAGIC_GEN1A,     // Gen1a (CUID) - backdoor auth
    MFC_MAGIC_GEN2,      // Gen2 (FUID/CUID) - 直接写 block 0
} mfc_magic_type_t;

// 检测 magic 卡类型（非破坏性）
mfc_magic_type_t mfc_detect_magic(void);

// 工具：扇区号 -> 该扇区第一个块号 / 块数
uint8_t mfc_sector_first_block(uint8_t sector);
uint8_t mfc_sector_block_count(uint8_t sector);
uint8_t mfc_sector_count_for_type(pn532_card_type_t type);

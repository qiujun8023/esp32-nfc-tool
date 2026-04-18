#include "mifare_classic.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nfc_cancel.h"

static const char* TAG = "mfc";

// ISO 14443-3 / NXP Mifare Classic 命令码
#define MIFARE_AUTH_A 0x60
#define MIFARE_AUTH_B 0x61
#define MIFARE_READ   0x30
#define MIFARE_WRITE  0xA0

uint8_t mfc_sector_count_for_type(pn532_card_type_t type) {
    if (type == PN532_CARD_MIFARE_CLASSIC_4K) return MFC_4K_SECTORS;
    return MFC_1K_SECTORS;
}

// Mifare Classic 4K 布局: 前 32 扇区 4 块/扇区,之后 8 扇区 16 块/扇区
uint8_t mfc_sector_first_block(uint8_t sector) {
    if (sector < 32) return sector * 4;
    return 32 * 4 + (sector - 32) * 16;
}

uint8_t mfc_sector_block_count(uint8_t sector) {
    return sector < 32 ? 4 : 16;
}

esp_err_t mfc_authenticate(const uint8_t* uid, uint8_t uid_len, uint8_t block, mfc_key_type_t kt, const uint8_t* key) {
    uint8_t tx[2 + MFC_KEY_LEN + 10];
    uint8_t i = 0;
    tx[i++]   = (kt == MFC_KEY_A) ? MIFARE_AUTH_A : MIFARE_AUTH_B;
    tx[i++]   = block;
    memcpy(tx + i, key, MFC_KEY_LEN);
    i += MFC_KEY_LEN;
    memcpy(tx + i, uid, uid_len);
    i += uid_len;

    uint8_t rx[8];
    uint8_t rx_len = sizeof(rx);
    return pn532_in_data_exchange(tx, i, rx, &rx_len, 200);
}

esp_err_t mfc_read_block(uint8_t block, uint8_t out[MFC_BLOCK_LEN]) {
    uint8_t tx[2]  = {MIFARE_READ, block};
    uint8_t rx[32] = {0};
    uint8_t rx_len = sizeof(rx);
    esp_err_t err  = pn532_in_data_exchange(tx, sizeof(tx), rx, &rx_len, 200);
    if (err != ESP_OK) return err;
    if (rx_len < MFC_BLOCK_LEN) return ESP_FAIL;
    memcpy(out, rx, MFC_BLOCK_LEN);
    return ESP_OK;
}

esp_err_t mfc_write_block(uint8_t block, const uint8_t in[MFC_BLOCK_LEN]) {
    uint8_t tx[2 + MFC_BLOCK_LEN];
    tx[0] = MIFARE_WRITE;
    tx[1] = block;
    memcpy(tx + 2, in, MFC_BLOCK_LEN);
    uint8_t rx[8];
    uint8_t rx_len = sizeof(rx);
    return pn532_in_data_exchange(tx, sizeof(tx), rx, &rx_len, 200);
}

mfc_magic_type_t mfc_detect_magic(void) {
    // Gen1a (CUID/UFUID) 后门认证序列: 0x40 (halt bypass) + 0x43,两个都回 ACK 才算
    uint8_t cmd40[] = {0x40};
    uint8_t rx[8];
    uint8_t rx_len = sizeof(rx);

    pn532_target_t tgt;
    if (pn532_read_passive_target(&tgt, 500) != ESP_OK) return MFC_MAGIC_NONE;

    if (pn532_in_data_exchange(cmd40, 1, rx, &rx_len, 200) == ESP_OK) {
        uint8_t cmd43[] = {0x43};
        rx_len = sizeof(rx);
        if (pn532_in_data_exchange(cmd43, 1, rx, &rx_len, 200) == ESP_OK) {
            return MFC_MAGIC_GEN1A;
        }
    }

    // Gen2 卡只能通过"直接写 block 0"区分,无法非破坏性检测;此处只上报 Gen1a
    return MFC_MAGIC_NONE;
}

typedef enum {
    TRY_OK = 0,
    TRY_WRONG_KEY,
    TRY_CARD_LOST,
} try_res_t;

/*
 * Mifare 认证失败后卡进入 halted 状态，下一次 auth 前必须 re-select。
 * re-select 偶发超时（RF/时序抖动），静默跳过会让正确的 key 永远试不到，所以最多重试 3 次；
 * 都失败才认定 CARD_LOST。
 */
static try_res_t try_key(const pn532_target_t* tgt, uint8_t block, mfc_key_type_t kt, const uint8_t* key) {
    for (int attempt = 0; attempt < 3; attempt++) {
        pn532_target_t reselect;
        if (pn532_read_passive_target(&reselect, 300) == ESP_OK &&
            reselect.uid_len == tgt->uid_len &&
            memcmp(reselect.uid, tgt->uid, tgt->uid_len) == 0) {
            return mfc_authenticate(reselect.uid, reselect.uid_len, block, kt, key) == ESP_OK
                       ? TRY_OK
                       : TRY_WRONG_KEY;
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    return TRY_CARD_LOST;
}

esp_err_t mfc_full_read(const pn532_target_t* target,
                        const uint8_t (*keys)[MFC_KEY_LEN], size_t keys_count,
                        mfc_dump_t* dump,
                        mfc_progress_cb cb, void* user) {
    memset(dump, 0, sizeof(*dump));
    dump->target       = *target;
    dump->sector_count = mfc_sector_count_for_type(target->type);
    dump->block_count  = (target->type == PN532_CARD_MIFARE_CLASSIC_4K) ? MFC_4K_BLOCKS : MFC_1K_BLOCKS;

    for (uint8_t s = 0; s < dump->sector_count; s++) {
        if (nfc_cancel_pending()) {
            ESP_LOGW(TAG, "full_read cancelled at sector %u", s);
            return ESP_ERR_TIMEOUT;
        }
        uint8_t first  = mfc_sector_first_block(s);
        uint8_t blocks = mfc_sector_block_count(s);

        for (size_t k = 0; k < keys_count; k++) {
            if (nfc_cancel_pending()) return ESP_ERR_TIMEOUT;
            try_res_t r = try_key(target, first, MFC_KEY_A, keys[k]);
            if (r == TRY_CARD_LOST) {
                ESP_LOGW(TAG, "card lost at sector %u (key a)", s);
                return ESP_ERR_NOT_FOUND;
            }
            if (r == TRY_OK) {
                dump->key_a[s].found = true;
                memcpy(dump->key_a[s].key, keys[k], MFC_KEY_LEN);
                for (uint8_t b = 0; b < blocks; b++) {
                    if (mfc_read_block(first + b, dump->data[first + b]) == ESP_OK) {
                        dump->block_read[first + b] = true;
                    }
                }
                break;
            }
            // 字典较长,定期喂狗防止 task watchdog
            if ((k & 0x0F) == 0) vTaskDelay(pdMS_TO_TICKS(1));
        }

        // key B 补一轮,处理 B-only 权限扇区
        for (size_t k = 0; k < keys_count; k++) {
            if (nfc_cancel_pending()) return ESP_ERR_TIMEOUT;
            try_res_t r = try_key(target, first, MFC_KEY_B, keys[k]);
            if (r == TRY_CARD_LOST) {
                ESP_LOGW(TAG, "card lost at sector %u (key b)", s);
                return ESP_ERR_NOT_FOUND;
            }
            if (r == TRY_OK) {
                dump->key_b[s].found = true;
                memcpy(dump->key_b[s].key, keys[k], MFC_KEY_LEN);
                for (uint8_t b = 0; b < blocks; b++) {
                    if (!dump->block_read[first + b] &&
                        mfc_read_block(first + b, dump->data[first + b]) == ESP_OK) {
                        dump->block_read[first + b] = true;
                    }
                }
                break;
            }
            if ((k & 0x0F) == 0) vTaskDelay(pdMS_TO_TICKS(1));
        }

        // trailer 读出时 key A 恒为 0、key B 可能被屏蔽，用已知 key 填回，便于写回流程
        if (dump->key_a[s].found || dump->key_b[s].found) {
            uint8_t* trailer = dump->data[first + blocks - 1];
            if (dump->key_a[s].found) memcpy(trailer, dump->key_a[s].key, MFC_KEY_LEN);
            if (dump->key_b[s].found) memcpy(trailer + 10, dump->key_b[s].key, MFC_KEY_LEN);
        }

        if (cb) cb(s, dump->sector_count, dump->key_a[s].found, dump->key_b[s].found, user);
    }

    return ESP_OK;
}

esp_err_t mfc_full_write(const mfc_dump_t* dump,
                         mfc_progress_cb cb, void* user,
                         uint8_t* verify_fail_count) {
    uint8_t fails = 0;
    pn532_target_t cur;
    if (pn532_read_passive_target(&cur, 1000) != ESP_OK) return ESP_ERR_NOT_FOUND;

    for (uint8_t s = 0; s < dump->sector_count; s++) {
        if (nfc_cancel_pending()) {
            ESP_LOGW(TAG, "full_write cancelled at sector %u", s);
            if (verify_fail_count) *verify_fail_count = fails;
            return ESP_ERR_TIMEOUT;
        }
        const mfc_known_key_t* known = dump->key_a[s].found ? &dump->key_a[s] : &dump->key_b[s];
        if (!known->found) {
            if (cb) cb(s, dump->sector_count, false, false, user);
            continue;
        }
        mfc_key_type_t kt    = dump->key_a[s].found ? MFC_KEY_A : MFC_KEY_B;
        uint8_t        first = mfc_sector_first_block(s);
        uint8_t        blocks = mfc_sector_block_count(s);

        try_res_t r1 = try_key(&cur, first, kt, known->key);
        if (r1 == TRY_CARD_LOST) {
            ESP_LOGW(TAG, "full_write: card lost at sector %u", s);
            if (verify_fail_count) *verify_fail_count = fails;
            return ESP_ERR_NOT_FOUND;
        }
        if (r1 != TRY_OK) {
            if (cb) cb(s, dump->sector_count, false, false, user);
            continue;
        }
        // block 0 是厂商块,普通卡不可写
        uint8_t start = (s == 0) ? 1 : 0;
        for (uint8_t b = start; b < blocks; b++) {
            mfc_write_block(first + b, dump->data[first + b]);
        }

        // 回读验证: trailer 的 key A/B 读回通常被屏蔽,只能比 access bits + user byte (6..9)
        if (try_key(&cur, first, kt, known->key) == TRY_OK) {
            for (uint8_t b = start; b < blocks; b++) {
                uint8_t readback[MFC_BLOCK_LEN];
                bool is_trailer = (b == blocks - 1);
                if (mfc_read_block(first + b, readback) == ESP_OK) {
                    bool mismatch;
                    if (is_trailer) {
                        mismatch = memcmp(readback + 6, dump->data[first + b] + 6, 4) != 0;
                    } else {
                        mismatch = memcmp(readback, dump->data[first + b], MFC_BLOCK_LEN) != 0;
                    }
                    if (mismatch) fails++;
                } else {
                    fails++;
                }
            }
        }

        if (cb) cb(s, dump->sector_count, dump->key_a[s].found, dump->key_b[s].found, user);
    }
    if (verify_fail_count) *verify_fail_count = fails;
    return ESP_OK;
}

/**
 * @file keys_store.c
 * @brief 用户自定义 Mifare 密钥（NVS 存储）
 *
 * NVS key 命名："k%02d"，value 是 6 字节 blob
 * 计数存在 "n" 键
 */

#include "keys_store.h"

#include <string.h>

#include "config.h"
#include "default_keys.h"
#include "nvs.h"

esp_err_t keys_store_init(void) {
    return ESP_OK;
}

static esp_err_t open_ns(nvs_handle_t* h, nvs_open_mode_t mode) {
    return nvs_open(NVS_NS_KEYS, mode, h);
}

size_t keys_store_load_user(uint8_t out[][MFC_KEY_LEN], size_t max_n) {
    nvs_handle_t h;
    if (open_ns(&h, NVS_READONLY) != ESP_OK) return 0;
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    if (n > max_n) n = max_n;
    for (uint8_t i = 0; i < n; i++) {
        char key[8];
        snprintf(key, sizeof(key), "k%02d", i);
        size_t blob_len = MFC_KEY_LEN;
        nvs_get_blob(h, key, out[i], &blob_len);
    }
    nvs_close(h);
    return n;
}

esp_err_t keys_store_add(const uint8_t key[MFC_KEY_LEN]) {
    nvs_handle_t h;
    esp_err_t    err = open_ns(&h, NVS_READWRITE);
    if (err != ESP_OK) return err;
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    if (n >= KEYS_USER_MAX) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    // 去重
    for (uint8_t i = 0; i < n; i++) {
        char   k[8];
        snprintf(k, sizeof(k), "k%02d", i);
        uint8_t exist[MFC_KEY_LEN];
        size_t  l = MFC_KEY_LEN;
        if (nvs_get_blob(h, k, exist, &l) == ESP_OK && memcmp(exist, key, MFC_KEY_LEN) == 0) {
            nvs_close(h);
            return ESP_OK;  // 已存在
        }
    }
    char k[8];
    snprintf(k, sizeof(k), "k%02d", n);
    err = nvs_set_blob(h, k, key, MFC_KEY_LEN);
    if (err == ESP_OK) {
        n++;
        nvs_set_u8(h, "n", n);
        nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t keys_store_remove(size_t index) {
    nvs_handle_t h;
    if (open_ns(&h, NVS_READWRITE) != ESP_OK) return ESP_FAIL;
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    if (index >= n) {
        nvs_close(h);
        return ESP_ERR_INVALID_ARG;
    }
    // 把后面的往前搬
    for (uint8_t i = (uint8_t)index; i < n - 1; i++) {
        char   k1[8], k2[8];
        snprintf(k1, sizeof(k1), "k%02d", i);
        snprintf(k2, sizeof(k2), "k%02d", i + 1);
        uint8_t buf[MFC_KEY_LEN];
        size_t  l = MFC_KEY_LEN;  // 每次迭代重置
        if (nvs_get_blob(h, k2, buf, &l) != ESP_OK) continue;
        nvs_set_blob(h, k1, buf, MFC_KEY_LEN);
    }
    char klast[8];
    snprintf(klast, sizeof(klast), "k%02d", n - 1);
    nvs_erase_key(h, klast);
    n--;
    nvs_set_u8(h, "n", n);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

size_t keys_store_combined(uint8_t out[][MFC_KEY_LEN], size_t max_n) {
    size_t n = keys_store_load_user(out, max_n);
    for (size_t i = 0; i < MIFARE_DEFAULT_KEY_COUNT && n < max_n; i++) {
        memcpy(out[n++], MIFARE_DEFAULT_KEYS[i], MFC_KEY_LEN);
    }
    return n;
}

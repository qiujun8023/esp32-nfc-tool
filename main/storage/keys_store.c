// NVS 存 key 序列: "n" 存数量 (u8), "k%02d" 存 6 字节 blob
// 搬移式 remove 会产生多次 set_blob, 在掉电时可能只写入部分, 但最多
// 丢失少量 key, 用户侧可重新添加, 不值得引入额外的事务层

#include "keys_store.h"

#include <string.h>

#include "config.h"
#include "default_keys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static SemaphoreHandle_t s_lock = NULL;

esp_err_t keys_store_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

#define LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

static esp_err_t open_ns(nvs_handle_t* h, nvs_open_mode_t mode) {
    return nvs_open(NVS_NS_KEYS, mode, h);
}

size_t keys_store_load_user(uint8_t out[][MFC_KEY_LEN], size_t max_n) {
    LOCK();
    nvs_handle_t h;
    if (open_ns(&h, NVS_READONLY) != ESP_OK) { UNLOCK(); return 0; }
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
    UNLOCK();
    return n;
}

esp_err_t keys_store_add(const uint8_t key[MFC_KEY_LEN]) {
    LOCK();
    nvs_handle_t h;
    esp_err_t    err = open_ns(&h, NVS_READWRITE);
    if (err != ESP_OK) { UNLOCK(); return err; }
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    if (n >= KEYS_USER_MAX) {
        nvs_close(h);
        UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    // 去重: 避免 UI 重复提交撑爆 64 上限
    for (uint8_t i = 0; i < n; i++) {
        char   k[8];
        snprintf(k, sizeof(k), "k%02d", i);
        uint8_t exist[MFC_KEY_LEN];
        size_t  l = MFC_KEY_LEN;
        if (nvs_get_blob(h, k, exist, &l) == ESP_OK && memcmp(exist, key, MFC_KEY_LEN) == 0) {
            nvs_close(h);
            UNLOCK();
            return ESP_OK;
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
    UNLOCK();
    return err;
}

esp_err_t keys_store_remove(size_t index) {
    LOCK();
    nvs_handle_t h;
    if (open_ns(&h, NVS_READWRITE) != ESP_OK) { UNLOCK(); return ESP_FAIL; }
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    if (index >= n) {
        nvs_close(h);
        UNLOCK();
        return ESP_ERR_INVALID_ARG;
    }
    // 整体前移, 保证索引连续 (UI 按索引删除)
    for (uint8_t i = (uint8_t)index; i < n - 1; i++) {
        char   k1[8], k2[8];
        snprintf(k1, sizeof(k1), "k%02d", i);
        snprintf(k2, sizeof(k2), "k%02d", i + 1);
        uint8_t buf[MFC_KEY_LEN];
        size_t  l = MFC_KEY_LEN;
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
    UNLOCK();
    return ESP_OK;
}

size_t keys_store_combined(uint8_t out[][MFC_KEY_LEN], size_t max_n) {
    // 用户 key 在前: 一旦命中就不必再走完整个默认字典,省爆破时间
    size_t n = keys_store_load_user(out, max_n);
    for (size_t i = 0; i < MIFARE_DEFAULT_KEY_COUNT && n < max_n; i++) {
        memcpy(out[n++], MIFARE_DEFAULT_KEYS[i], MFC_KEY_LEN);
    }
    return n;
}

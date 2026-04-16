/**
 * @file dump_store.c
 * @brief LittleFS 上的 dump 文件管理
 *
 * 文件命名：<counter>_<uid_hex>
 *   .bin  原始数据（mfc：64*16=1024 或 256*16=4096；ntag：N*4）
 *   .json 元数据
 *
 * 计数器存储在 NVS "dump_cnt" 命名空间中，自增保证文件名有序。
 */

#include "dump_store.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "config.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "dump_store";

#define NVS_NS_DUMP "dump_cnt"
#define NVS_KEY_CNT "cnt"

static uint32_t next_counter(void) {
    nvs_handle_t h;
    uint32_t cnt = 0;
    if (nvs_open(NVS_NS_DUMP, NVS_READWRITE, &h) != ESP_OK) return 0;
    nvs_get_u32(h, NVS_KEY_CNT, &cnt);
    cnt++;
    nvs_set_u32(h, NVS_KEY_CNT, cnt);
    nvs_commit(h);
    nvs_close(h);
    return cnt;
}

esp_err_t dump_store_init(void) {
    esp_vfs_littlefs_conf_t cfg = {
        .base_path              = LFS_BASE,
        .partition_label        = LFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }
    mkdir(DUMPS_DIR, 0755);
    size_t total = 0, used = 0;
    esp_littlefs_info(LFS_PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "littlefs mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

static void uid_to_hex(const uint8_t* uid, uint8_t len, char* out) {
    static const char H[] = "0123456789abcdef";
    for (uint8_t i = 0; i < len; i++) {
        out[i * 2]     = H[uid[i] >> 4];
        out[i * 2 + 1] = H[uid[i] & 0xF];
    }
    out[len * 2] = 0;
}

static void make_id(char id[40], const uint8_t* uid, uint8_t uid_len, uint32_t cnt) {
    char hex[24];
    uid_to_hex(uid, uid_len, hex);
    snprintf(id, 40, "%04lu_%s", (unsigned long)cnt, hex);
}

static void path_for(char* out, size_t outlen, const char* id, const char* ext) {
    snprintf(out, outlen, "%s/%s.%s", DUMPS_DIR, id, ext);
}

static esp_err_t write_meta_json(const char* id, const dump_meta_t* m) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", m->id);
    cJSON_AddStringToObject(root, "name", m->name);
    cJSON_AddNumberToObject(root, "type", m->type);
    char hex[24];
    uid_to_hex(m->uid, m->uid_len, hex);
    cJSON_AddStringToObject(root, "uid", hex);
    cJSON_AddNumberToObject(root, "atqa", m->atqa);
    cJSON_AddNumberToObject(root, "sak", m->sak);
    cJSON_AddNumberToObject(root, "blocks", m->block_or_page_count);
    cJSON_AddNumberToObject(root, "knownKeys", m->known_keys);
    cJSON_AddNumberToObject(root, "seq", m->seq);
    cJSON_AddNumberToObject(root, "binSize", m->bin_size);

    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char path[128];
    path_for(path, sizeof(path), id, "json");
    FILE* f = fopen(path, "w");
    if (!f) {
        free(str);
        return ESP_FAIL;
    }
    fputs(str, f);
    fclose(f);
    free(str);
    return ESP_OK;
}

esp_err_t dump_store_save_mfc(const mfc_dump_t* d, const char* name, char out_id[40]) {
    uint32_t cnt = next_counter();
    char id[40];
    make_id(id, d->target.uid, d->target.uid_len, cnt);

    char path[128];
    path_for(path, sizeof(path), id, "bin");
    FILE* f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t bin_size = (size_t)d->block_count * MFC_BLOCK_LEN;
    fwrite(d->data, 1, bin_size, f);
    fclose(f);

    dump_meta_t m       = {0};
    strlcpy(m.id, id, sizeof(m.id));
    strlcpy(m.name, (name && *name) ? name : "未命名", sizeof(m.name));
    m.type                = DUMP_TYPE_MIFARE_CLASSIC;
    m.uid_len             = d->target.uid_len;
    memcpy(m.uid, d->target.uid, d->target.uid_len);
    m.atqa                = d->target.atqa;
    m.sak                 = d->target.sak;
    m.block_or_page_count = d->block_count;
    m.seq                 = cnt;
    m.bin_size            = bin_size;
    for (uint8_t s = 0; s < d->sector_count; s++) {
        if (d->key_a[s].found) m.known_keys++;
        if (d->key_b[s].found) m.known_keys++;
    }
    write_meta_json(id, &m);
    if (out_id) strlcpy(out_id, id, 40);
    return ESP_OK;
}

esp_err_t dump_store_save_ntag(const ntag_dump_t* d, const pn532_target_t* tgt, const char* name, char out_id[40]) {
    uint32_t cnt = next_counter();
    char id[40];
    make_id(id, tgt->uid, tgt->uid_len, cnt);

    char path[128];
    path_for(path, sizeof(path), id, "bin");
    FILE* f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t bin_size = (size_t)d->total_pages * NTAG_PAGE_SIZE;
    fwrite(d->pages, 1, bin_size, f);
    fclose(f);

    dump_meta_t m = {0};
    strlcpy(m.id, id, sizeof(m.id));
    strlcpy(m.name, (name && *name) ? name : "未命名", sizeof(m.name));
    m.type                = DUMP_TYPE_NTAG;
    m.uid_len             = tgt->uid_len;
    memcpy(m.uid, tgt->uid, tgt->uid_len);
    m.atqa                = tgt->atqa;
    m.sak                 = tgt->sak;
    m.block_or_page_count = d->total_pages;
    m.seq                 = cnt;
    m.bin_size            = bin_size;
    write_meta_json(id, &m);
    if (out_id) strlcpy(out_id, id, 40);
    return ESP_OK;
}

static bool parse_meta(const char* json_path, dump_meta_t* out) {
    FILE* f = fopen(json_path, "r");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2048) {
        fclose(f);
        return false;
    }
    char* buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    memset(out, 0, sizeof(*out));
    const cJSON* j;
    if ((j = cJSON_GetObjectItem(root, "id")) && cJSON_IsString(j)) strlcpy(out->id, j->valuestring, sizeof(out->id));
    if ((j = cJSON_GetObjectItem(root, "name")) && cJSON_IsString(j)) strlcpy(out->name, j->valuestring, sizeof(out->name));
    if ((j = cJSON_GetObjectItem(root, "type"))) out->type = (dump_type_t)j->valueint;
    if ((j = cJSON_GetObjectItem(root, "uid")) && cJSON_IsString(j)) {
        size_t hexlen     = strlen(j->valuestring);
        out->uid_len      = hexlen / 2;
        if (out->uid_len > 10) out->uid_len = 10;
        for (uint8_t i = 0; i < out->uid_len; i++) {
            char c[3] = {j->valuestring[i * 2], j->valuestring[i * 2 + 1], 0};
            out->uid[i] = (uint8_t)strtoul(c, NULL, 16);
        }
    }
    if ((j = cJSON_GetObjectItem(root, "atqa"))) out->atqa = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "sak"))) out->sak = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "blocks"))) out->block_or_page_count = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "knownKeys"))) out->known_keys = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "seq"))) out->seq = (uint32_t)j->valuedouble;
    // 兼容旧格式：如果有 createdAt 但没有 seq，用 createdAt 作为 seq
    if (!out->seq && (j = cJSON_GetObjectItem(root, "createdAt"))) out->seq = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "binSize"))) out->bin_size = j->valueint;
    cJSON_Delete(root);
    return true;
}

size_t dump_store_list(dump_meta_t* out, size_t max_n) {
    DIR* d = opendir(DUMPS_DIR);
    if (!d) return 0;
    size_t n = 0;
    struct dirent* e;
    while ((e = readdir(d)) && n < max_n) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 5, ".json") != 0) continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", DUMPS_DIR, e->d_name);
        if (parse_meta(path, &out[n])) n++;
    }
    closedir(d);
    return n;
}

esp_err_t dump_store_get_meta(const char* id, dump_meta_t* meta) {
    char path[128];
    path_for(path, sizeof(path), id, "json");
    return parse_meta(path, meta) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t dump_store_read_bin(const char* id, uint8_t** buf, size_t* len) {
    char path[128];
    path_for(path, sizeof(path), id, "bin");
    FILE* f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return ESP_FAIL;
    }
    *buf = malloc(sz);
    if (!*buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fread(*buf, 1, sz, f);
    fclose(f);
    *len = sz;
    return ESP_OK;
}

esp_err_t dump_store_load_mfc(const char* id, mfc_dump_t* dump) {
    dump_meta_t m;
    if (dump_store_get_meta(id, &m) != ESP_OK) return ESP_ERR_NOT_FOUND;
    if (m.type != DUMP_TYPE_MIFARE_CLASSIC) return ESP_ERR_INVALID_ARG;

    uint8_t* bin = NULL;
    size_t   bin_len;
    if (dump_store_read_bin(id, &bin, &bin_len) != ESP_OK) return ESP_FAIL;

    memset(dump, 0, sizeof(*dump));
    dump->target.uid_len = m.uid_len;
    memcpy(dump->target.uid, m.uid, m.uid_len);
    dump->target.atqa = m.atqa;
    dump->target.sak  = m.sak;
    dump->target.type = (m.block_or_page_count > 64) ? PN532_CARD_MIFARE_CLASSIC_4K : PN532_CARD_MIFARE_CLASSIC_1K;
    dump->sector_count = mfc_sector_count_for_type(dump->target.type);
    dump->block_count  = m.block_or_page_count;
    size_t copy_len    = bin_len > sizeof(dump->data) ? sizeof(dump->data) : bin_len;
    memcpy(dump->data, bin, copy_len);
    free(bin);

    // 从 sector trailer 恢复已知 key
    for (uint8_t s = 0; s < dump->sector_count; s++) {
        uint8_t  trailer_blk = mfc_sector_first_block(s) + mfc_sector_block_count(s) - 1;
        uint8_t* t           = dump->data[trailer_blk];
        bool     a_zero = true, b_zero = true;
        for (int i = 0; i < 6; i++) {
            if (t[i]) a_zero = false;
            if (t[10 + i]) b_zero = false;
        }
        if (!a_zero) {
            dump->key_a[s].found = true;
            memcpy(dump->key_a[s].key, t, 6);
        }
        if (!b_zero) {
            dump->key_b[s].found = true;
            memcpy(dump->key_b[s].key, t + 10, 6);
        }
        for (uint8_t b = 0; b < mfc_sector_block_count(s); b++) {
            dump->block_read[mfc_sector_first_block(s) + b] = true;
        }
    }
    return ESP_OK;
}

esp_err_t dump_store_delete(const char* id) {
    char path[128];
    path_for(path, sizeof(path), id, "bin");
    remove(path);
    path_for(path, sizeof(path), id, "json");
    remove(path);
    return ESP_OK;
}

esp_err_t dump_store_rename(const char* id, const char* new_name) {
    dump_meta_t m;
    if (dump_store_get_meta(id, &m) != ESP_OK) return ESP_ERR_NOT_FOUND;
    strlcpy(m.name, new_name, sizeof(m.name));
    return write_meta_json(id, &m);
}

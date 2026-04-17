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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "config.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "hex_util.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "dump_store";

#define NVS_NS_DUMP "dump_cnt"
#define NVS_KEY_CNT "cnt"

static bool parse_meta(const char* json_path, dump_meta_t* out);

// 分配并 next_counter 原子递增。任一 NVS 调用失败时返回 0，表示调用方应放弃保存。
static uint32_t next_counter(void) {
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NS_DUMP, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open cnt failed: %s", esp_err_to_name(err));
        return 0;
    }
    uint32_t cnt = 0;
    nvs_get_u32(h, NVS_KEY_CNT, &cnt);  // 不存在视为 0
    cnt++;
    err = nvs_set_u32(h, NVS_KEY_CNT, cnt);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write cnt failed: %s", esp_err_to_name(err));
        return 0;
    }
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

static void make_id(char id[40], const uint8_t* uid, uint8_t uid_len, uint32_t cnt) {
    char hex[24];
    hex_encode_lower(uid, uid_len, hex);
    snprintf(id, 40, "%04lu_%s", (unsigned long)cnt, hex);
}

static void path_for(char* out, size_t outlen, const char* id, const char* ext) {
    snprintf(out, outlen, "%s/%s.%s", DUMPS_DIR, id, ext);
}

// 写 bin 文件，失败会删除部分写入产物。
static esp_err_t write_bin(const char* id, const void* data, size_t len) {
    char path[128];
    path_for(path, sizeof(path), id, "bin");
    FILE* f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t w = fwrite(data, 1, len, f);
    int    cerr = fclose(f);
    if (w != len || cerr != 0) {
        ESP_LOGE(TAG, "write %s short: %u/%u", path, (unsigned)w, (unsigned)len);
        remove(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_meta_json(const char* id, const dump_meta_t* m) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", m->id);
    cJSON_AddStringToObject(root, "name", m->name);
    cJSON_AddNumberToObject(root, "type", m->type);
    char hex[24];
    hex_encode_lower(m->uid, m->uid_len, hex);
    cJSON_AddStringToObject(root, "uid", hex);
    cJSON_AddNumberToObject(root, "atqa", m->atqa);
    cJSON_AddNumberToObject(root, "sak", m->sak);
    cJSON_AddNumberToObject(root, "blocks", m->block_or_page_count);
    cJSON_AddNumberToObject(root, "knownKeys", m->known_keys);
    cJSON_AddNumberToObject(root, "seq", m->seq);
    cJSON_AddNumberToObject(root, "binSize", m->bin_size);
    if (m->created_ms > 0) {
        cJSON_AddNumberToObject(root, "createdMs", (double)m->created_ms);
    }

    char* str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!str) return ESP_ERR_NO_MEM;

    char path[128];
    path_for(path, sizeof(path), id, "json");
    FILE* f = fopen(path, "w");
    if (!f) {
        free(str);
        return ESP_FAIL;
    }
    size_t n    = strlen(str);
    size_t w    = fwrite(str, 1, n, f);
    int    cerr = fclose(f);
    free(str);
    if (w != n || cerr != 0) {
        ESP_LOGE(TAG, "write %s short: %u/%u", path, (unsigned)w, (unsigned)n);
        remove(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*
 * 查找同 (uid, type) 的已有 dump。存在时用 out_id/out_name 返回；用于在保存前去重。
 * 不使用 dump_store_list 以避免一次性分配 128 条元数据。
 */
static bool find_existing_by_uid_type(const uint8_t* uid, uint8_t uid_len, dump_type_t type,
                                      char out_id[40], char out_name[64]) {
    DIR* d = opendir(DUMPS_DIR);
    if (!d) return false;
    struct dirent* e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        size_t nlen = strlen(e->d_name);
        if (nlen < 5 || strcmp(e->d_name + nlen - 5, ".json") != 0) continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", DUMPS_DIR, e->d_name);
        dump_meta_t m;
        if (!parse_meta(path, &m)) continue;
        if (m.type == type && m.uid_len == uid_len && memcmp(m.uid, uid, uid_len) == 0) {
            strlcpy(out_id, m.id, 40);
            if (out_name) strlcpy(out_name, m.name, 64);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/*
 * 把新扫描 upd 的已读块 / 已知 key 并入 base（base 来自旧 dump）。
 * 原则：upd 有的覆盖 base；upd 没读到的保留 base。
 * 合并后把 key 同步回 trailer 镜像，保证下次 load 能恢复。
 */
static void merge_mfc_into(mfc_dump_t* base, const mfc_dump_t* upd) {
    base->target = upd->target;  // atqa/sak/uid 按最新一次扫描
    if (upd->sector_count > base->sector_count) base->sector_count = upd->sector_count;
    if (upd->block_count  > base->block_count)  base->block_count  = upd->block_count;

    for (uint16_t b = 0; b < upd->block_count && b < 256; b++) {
        if (upd->block_read[b]) {
            memcpy(base->data[b], upd->data[b], MFC_BLOCK_LEN);
            base->block_read[b] = true;
        }
    }
    for (uint8_t s = 0; s < upd->sector_count && s < MFC_MAX_SECTORS; s++) {
        if (upd->key_a[s].found) base->key_a[s] = upd->key_a[s];
        if (upd->key_b[s].found) base->key_b[s] = upd->key_b[s];
    }
    for (uint8_t s = 0; s < base->sector_count; s++) {
        uint8_t  first   = mfc_sector_first_block(s);
        uint8_t  blocks  = mfc_sector_block_count(s);
        uint8_t* trailer = base->data[first + blocks - 1];
        if (base->key_a[s].found) memcpy(trailer,      base->key_a[s].key, MFC_KEY_LEN);
        if (base->key_b[s].found) memcpy(trailer + 10, base->key_b[s].key, MFC_KEY_LEN);
    }
}

esp_err_t dump_store_save_mfc(const mfc_dump_t* d, const char* name, int64_t created_ms, char out_id[40]) {
    // UID 去重合并：旧 dump 视为 base，新扫描 d 覆盖到上面
    char old_id[40]   = {0};
    char old_name[64] = {0};
    const mfc_dump_t* to_save = d;
    mfc_dump_t* merged        = NULL;

    bool has_old = find_existing_by_uid_type(d->target.uid, d->target.uid_len,
                                             DUMP_TYPE_MIFARE_CLASSIC, old_id, old_name);
    if (has_old) {
        merged = calloc(1, sizeof(mfc_dump_t));
        if (merged && dump_store_load_mfc(old_id, merged) == ESP_OK) {
            merge_mfc_into(merged, d);
            to_save = merged;
        } else {
            free(merged);
            merged = NULL;
        }
        dump_store_delete(old_id);
        if (old_name[0] && strcmp(old_name, "未命名") != 0) name = old_name;
    }

    uint32_t cnt = next_counter();
    if (cnt == 0) { free(merged); return ESP_FAIL; }
    char id[40];
    make_id(id, to_save->target.uid, to_save->target.uid_len, cnt);

    size_t bin_size = (size_t)to_save->block_count * MFC_BLOCK_LEN;
    esp_err_t err = write_bin(id, to_save->data, bin_size);
    if (err != ESP_OK) { free(merged); return err; }

    dump_meta_t m = {0};
    strlcpy(m.id, id, sizeof(m.id));
    strlcpy(m.name, (name && *name) ? name : "未命名", sizeof(m.name));
    m.type                = DUMP_TYPE_MIFARE_CLASSIC;
    m.uid_len             = to_save->target.uid_len;
    memcpy(m.uid, to_save->target.uid, to_save->target.uid_len);
    m.atqa                = to_save->target.atqa;
    m.sak                 = to_save->target.sak;
    m.block_or_page_count = to_save->block_count;
    m.seq                 = cnt;
    m.bin_size            = bin_size;
    m.created_ms          = created_ms > 0 ? created_ms : 0;
    for (uint8_t s = 0; s < to_save->sector_count; s++) {
        if (to_save->key_a[s].found) m.known_keys++;
        if (to_save->key_b[s].found) m.known_keys++;
    }
    err = write_meta_json(id, &m);
    if (err != ESP_OK) {
        char path[128];
        path_for(path, sizeof(path), id, "bin");
        remove(path);
        free(merged);
        return err;
    }
    if (out_id) strlcpy(out_id, id, 40);
    free(merged);
    return ESP_OK;
}

esp_err_t dump_store_save_ntag(const ntag_dump_t* d, const pn532_target_t* tgt, const char* name, int64_t created_ms, char out_id[40]) {
    // UID 去重合并：旧 NTAG dump 的所有页视为已读，新读到的覆盖之
    char old_id[40]   = {0};
    char old_name[64] = {0};
    const ntag_dump_t* to_save = d;
    ntag_dump_t* merged        = NULL;

    bool has_old = find_existing_by_uid_type(tgt->uid, tgt->uid_len,
                                             DUMP_TYPE_NTAG, old_id, old_name);
    if (has_old) {
        uint8_t* old_bin = NULL;
        size_t   old_len = 0;
        if (dump_store_read_bin(old_id, &old_bin, &old_len) == ESP_OK) {
            merged = calloc(1, sizeof(ntag_dump_t));
            if (merged) {
                uint16_t old_pages = old_len / NTAG_PAGE_SIZE;
                if (old_pages > 256) old_pages = 256;
                merged->total_pages = old_pages;
                memcpy(merged->pages, old_bin, (size_t)old_pages * NTAG_PAGE_SIZE);
                for (uint16_t p = 0; p < old_pages; p++) merged->page_read[p] = true;
                merged->type = d->type;

                if (d->total_pages > merged->total_pages) merged->total_pages = d->total_pages;
                for (uint16_t p = 0; p < d->total_pages && p < 256; p++) {
                    if (d->page_read[p]) {
                        memcpy(merged->pages[p], d->pages[p], NTAG_PAGE_SIZE);
                        merged->page_read[p] = true;
                    }
                }
                to_save = merged;
            }
            free(old_bin);
        }
        dump_store_delete(old_id);
        if (old_name[0] && strcmp(old_name, "未命名") != 0) name = old_name;
    }

    uint32_t cnt = next_counter();
    if (cnt == 0) { free(merged); return ESP_FAIL; }
    char id[40];
    make_id(id, tgt->uid, tgt->uid_len, cnt);

    size_t bin_size = (size_t)to_save->total_pages * NTAG_PAGE_SIZE;
    esp_err_t err = write_bin(id, to_save->pages, bin_size);
    if (err != ESP_OK) { free(merged); return err; }

    dump_meta_t m = {0};
    strlcpy(m.id, id, sizeof(m.id));
    strlcpy(m.name, (name && *name) ? name : "未命名", sizeof(m.name));
    m.type                = DUMP_TYPE_NTAG;
    m.uid_len             = tgt->uid_len;
    memcpy(m.uid, tgt->uid, tgt->uid_len);
    m.atqa                = tgt->atqa;
    m.sak                 = tgt->sak;
    m.block_or_page_count = to_save->total_pages;
    m.seq                 = cnt;
    m.bin_size            = bin_size;
    m.created_ms          = created_ms > 0 ? created_ms : 0;
    err = write_meta_json(id, &m);
    if (err != ESP_OK) {
        char path[128];
        path_for(path, sizeof(path), id, "bin");
        remove(path);
        free(merged);
        return err;
    }
    if (out_id) strlcpy(out_id, id, 40);
    free(merged);
    return ESP_OK;
}

static bool parse_meta(const char* json_path, dump_meta_t* out) {
    FILE* f = fopen(json_path, "r");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 2048) {
        fclose(f);
        return false;
    }
    char* buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return false;
    }
    buf[sz] = 0;

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    memset(out, 0, sizeof(*out));
    const cJSON* j;
    if ((j = cJSON_GetObjectItem(root, "id")) && cJSON_IsString(j)) strlcpy(out->id, j->valuestring, sizeof(out->id));
    if ((j = cJSON_GetObjectItem(root, "name")) && cJSON_IsString(j)) strlcpy(out->name, j->valuestring, sizeof(out->name));
    if ((j = cJSON_GetObjectItem(root, "type"))) out->type = (dump_type_t)j->valueint;
    if ((j = cJSON_GetObjectItem(root, "uid")) && cJSON_IsString(j)) {
        size_t n = 0;
        hex_decode(j->valuestring, out->uid, sizeof(out->uid), &n, false);
        out->uid_len = (uint8_t)n;
    }
    if ((j = cJSON_GetObjectItem(root, "atqa"))) out->atqa = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "sak"))) out->sak = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "blocks"))) out->block_or_page_count = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "knownKeys"))) out->known_keys = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "seq"))) out->seq = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "binSize"))) out->bin_size = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "createdMs"))) out->created_ms = (int64_t)j->valuedouble;
    cJSON_Delete(root);
    return true;
}

static int cmp_meta_by_seq_desc(const void* a, const void* b) {
    uint32_t sa = ((const dump_meta_t*)a)->seq;
    uint32_t sb = ((const dump_meta_t*)b)->seq;
    if (sa < sb) return 1;
    if (sa > sb) return -1;
    return 0;
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
    if (n > 1) qsort(out, n, sizeof(dump_meta_t), cmp_meta_by_seq_desc);
    return n;
}

esp_err_t dump_store_get_meta(const char* id, dump_meta_t* meta) {
    if (!id_is_safe(id, 39)) return ESP_ERR_INVALID_ARG;
    char path[128];
    path_for(path, sizeof(path), id, "json");
    return parse_meta(path, meta) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t dump_store_read_bin(const char* id, uint8_t** buf, size_t* len) {
    if (!id_is_safe(id, 39) || !buf || !len) return ESP_ERR_INVALID_ARG;
    char path[128];
    path_for(path, sizeof(path), id, "bin");
    FILE* f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        return ESP_FAIL;
    }
    uint8_t* p = malloc(sz);
    if (!p) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(p, 1, sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(p);
        return ESP_FAIL;
    }
    *buf = p;
    *len = sz;
    return ESP_OK;
}

void dump_store_recover_trailer_keys(mfc_dump_t* dump) {
    /* 全零 key 合法，无法据此判断 trailer 是否读取过；改看 access bits 非零 */
    for (uint8_t s = 0; s < dump->sector_count; s++) {
        uint8_t  trailer_blk = mfc_sector_first_block(s) + mfc_sector_block_count(s) - 1;
        uint8_t* t           = dump->data[trailer_blk];
        bool trailer_valid = (t[6] | t[7] | t[8]) != 0;
        if (trailer_valid) {
            dump->key_a[s].found = true;
            memcpy(dump->key_a[s].key, t, 6);
            dump->key_b[s].found = true;
            memcpy(dump->key_b[s].key, t + 10, 6);
        }
        for (uint8_t b = 0; b < mfc_sector_block_count(s); b++) {
            dump->block_read[mfc_sector_first_block(s) + b] = true;
        }
    }
}

esp_err_t dump_store_load_mfc(const char* id, mfc_dump_t* dump) {
    dump_meta_t m;
    esp_err_t   err = dump_store_get_meta(id, &m);
    if (err != ESP_OK) return err;
    if (m.type != DUMP_TYPE_MIFARE_CLASSIC) return ESP_ERR_INVALID_ARG;

    uint8_t* bin = NULL;
    size_t   bin_len;
    if (dump_store_read_bin(id, &bin, &bin_len) != ESP_OK) return ESP_FAIL;

    memset(dump, 0, sizeof(*dump));
    dump->target.uid_len = m.uid_len;
    memcpy(dump->target.uid, m.uid, m.uid_len);
    dump->target.atqa = m.atqa;
    dump->target.sak  = m.sak;
    // 严格按块数判定卡型：64 = 1K，256 = 4K；其他按规模归类。
    bool is_4k = (m.block_or_page_count >= 256) || (bin_len >= 4096);
    dump->target.type  = is_4k ? PN532_CARD_MIFARE_CLASSIC_4K : PN532_CARD_MIFARE_CLASSIC_1K;
    dump->sector_count = mfc_sector_count_for_type(dump->target.type);
    dump->block_count  = is_4k ? 256 : 64;

    size_t copy_len = bin_len > sizeof(dump->data) ? sizeof(dump->data) : bin_len;
    memcpy(dump->data, bin, copy_len);
    free(bin);

    dump_store_recover_trailer_keys(dump);
    return ESP_OK;
}

esp_err_t dump_store_delete(const char* id) {
    if (!id_is_safe(id, 39)) return ESP_ERR_INVALID_ARG;
    char path[128];
    path_for(path, sizeof(path), id, "bin");
    remove(path);
    path_for(path, sizeof(path), id, "json");
    remove(path);
    return ESP_OK;
}

esp_err_t dump_store_rename(const char* id, const char* new_name) {
    if (!id_is_safe(id, 39)) return ESP_ERR_INVALID_ARG;
    dump_meta_t m;
    if (dump_store_get_meta(id, &m) != ESP_OK) return ESP_ERR_NOT_FOUND;
    strlcpy(m.name, new_name ? new_name : "", sizeof(m.name));
    return write_meta_json(id, &m);
}

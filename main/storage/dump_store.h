#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mifare_classic.h"
#include "ntag.h"

typedef enum {
    DUMP_TYPE_MIFARE_CLASSIC = 1,
    DUMP_TYPE_NTAG           = 2,
} dump_type_t;

typedef struct {
    char        id[40];   // 同时充当 URL 路径段与文件名（无扩展名）
    char        name[64]; // 用户备注
    dump_type_t type;
    uint8_t     uid[10];
    uint8_t     uid_len;
    uint16_t    atqa;
    uint8_t     sak;
    uint16_t    block_or_page_count;
    uint8_t     known_keys;
    uint32_t    seq;        // 排序用, 递增不保证连续
    uint16_t    bin_size;
    int64_t     created_ms; // 设备无 RTC, 由浏览器上传时带; 0 = 未知
} dump_meta_t;

esp_err_t dump_store_init(void);

// created_ms<=0 表示未知（设备无 RTC）
esp_err_t dump_store_save_mfc(const mfc_dump_t* d, const char* name, int64_t created_ms, char out_id[40]);
esp_err_t dump_store_save_ntag(const ntag_dump_t* d, const pn532_target_t* tgt, const char* name, int64_t created_ms, char out_id[40]);

size_t dump_store_list(dump_meta_t* out, size_t max_n);

esp_err_t dump_store_get_meta(const char* id, dump_meta_t* meta);

// *buf 由调用方 free
esp_err_t dump_store_read_bin(const char* id, uint8_t** buf, size_t* len);

esp_err_t dump_store_load_mfc(const char* id, mfc_dump_t* dump);

// load/upload 共用: 把可读 trailer 的 KeyA/B 回填到 dump->keys,
// 这样即便 json 元数据里 known 数不准, 回写时仍有 key 可用
void dump_store_recover_trailer_keys(mfc_dump_t* dump);

esp_err_t dump_store_delete(const char* id);
esp_err_t dump_store_rename(const char* id, const char* new_name);

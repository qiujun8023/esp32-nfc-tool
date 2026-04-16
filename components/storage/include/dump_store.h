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
    char        id[40];        // 文件名（不含扩展名），用于 URL
    char        name[64];      // 用户备注
    dump_type_t type;
    uint8_t     uid[10];
    uint8_t     uid_len;
    uint16_t    atqa;
    uint8_t     sak;
    uint16_t    block_or_page_count;  // mfc: 块数；ntag: 页数
    uint8_t     known_keys;           // mfc 已知 key 数量
    uint32_t    seq;                  // 自增序号（替代 unix 时间）
    uint16_t    bin_size;             // .bin 大小
} dump_meta_t;

esp_err_t dump_store_init(void);

// 保存 Mifare Classic dump（生成 .bin + .json）
esp_err_t dump_store_save_mfc(const mfc_dump_t* d, const char* name, char out_id[40]);
esp_err_t dump_store_save_ntag(const ntag_dump_t* d, const pn532_target_t* tgt, const char* name, char out_id[40]);

// 列出所有 dump（最多 max_n 条），返回实际数量
size_t dump_store_list(dump_meta_t* out, size_t max_n);

// 读取单个 dump 的元数据
esp_err_t dump_store_get_meta(const char* id, dump_meta_t* meta);

// 读取 dump 二进制（用户负责 free）
esp_err_t dump_store_read_bin(const char* id, uint8_t** buf, size_t* len);

// 读出完整 mfc dump（用于回写）
esp_err_t dump_store_load_mfc(const char* id, mfc_dump_t* dump);

esp_err_t dump_store_delete(const char* id);
esp_err_t dump_store_rename(const char* id, const char* new_name);

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pn532.h"

#define NTAG_PAGE_SIZE 4

typedef enum {
    NTAG_UNKNOWN = 0,
    NTAG_213 = 213,  // 144 字节用户区，45 页
    NTAG_215 = 215,  // 504 字节，135 页
    NTAG_216 = 216,  // 888 字节，231 页
} ntag_type_t;

typedef struct {
    ntag_type_t type;
    uint16_t    total_pages;
    uint8_t     pages[256][NTAG_PAGE_SIZE];
    bool        page_read[256];
} ntag_dump_t;

// 通过读 GET_VERSION 区分 NTAG 型号
ntag_type_t ntag_detect_type(void);

esp_err_t ntag_read_page(uint8_t page, uint8_t out[NTAG_PAGE_SIZE]);
esp_err_t ntag_write_page(uint8_t page, const uint8_t in[NTAG_PAGE_SIZE]);

esp_err_t ntag_full_read(const pn532_target_t* tgt, ntag_dump_t* dump);
esp_err_t ntag_full_write(const ntag_dump_t* dump);

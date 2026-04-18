#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pn532.h"

#define NTAG_PAGE_SIZE 4
#define NTAG_MAX_PAGES 256

typedef enum {
    NTAG_UNKNOWN = 0,
    NTAG_213 = 213,  // 45 页, 144 字节用户区
    NTAG_215 = 215,  // 135 页, 504 字节
    NTAG_216 = 216,  // 231 页, 888 字节
} ntag_type_t;

typedef struct {
    ntag_type_t type;
    uint16_t    total_pages;
    uint8_t     pages[NTAG_MAX_PAGES][NTAG_PAGE_SIZE];
    bool        page_read[NTAG_MAX_PAGES];
} ntag_dump_t;

// 基于 GET_VERSION 响应的 storage size code 推断型号
ntag_type_t ntag_detect_type(void);

esp_err_t ntag_read_page(uint8_t page, uint8_t out[NTAG_PAGE_SIZE]);
esp_err_t ntag_write_page(uint8_t page, const uint8_t in[NTAG_PAGE_SIZE]);

esp_err_t ntag_full_read(const pn532_target_t* tgt, ntag_dump_t* dump);
esp_err_t ntag_full_write(const ntag_dump_t* dump);

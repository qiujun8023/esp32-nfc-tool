#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ntag.h"

// NFC Forum URI RTD 1.0 前缀代码
#define NDEF_URI_NONE    0x00
#define NDEF_URI_HTTP_W  0x01  // http://www.
#define NDEF_URI_HTTPS_W 0x02  // https://www.
#define NDEF_URI_HTTP    0x03  // http://
#define NDEF_URI_HTTPS   0x04  // https://

typedef enum {
    NDEF_TYPE_UNKNOWN = 0,
    NDEF_TYPE_URI,
    NDEF_TYPE_TEXT,
} ndef_type_t;

typedef struct {
    ndef_type_t type;
    char        payload[512];
    char        lang[8];
} ndef_record_t;

// 只取 dump 中第一条 NDEF record
esp_err_t ndef_parse(const ntag_dump_t* dump, ndef_record_t* out);

// 自动匹配最长前缀码以压缩体积; uri 传完整 URL
esp_err_t ndef_write_uri(const char* uri);

esp_err_t ndef_write_text(const char* lang, const char* text);

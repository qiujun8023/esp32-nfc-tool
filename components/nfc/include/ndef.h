#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ntag.h"

// NDEF URI 协议前缀代码
#define NDEF_URI_NONE    0x00
#define NDEF_URI_HTTP_W  0x01  // http://www.
#define NDEF_URI_HTTPS_W 0x02  // https://www.
#define NDEF_URI_HTTP    0x03  // http://
#define NDEF_URI_HTTPS   0x04  // https://

// NDEF 记录类型
typedef enum {
    NDEF_TYPE_UNKNOWN = 0,
    NDEF_TYPE_URI,
    NDEF_TYPE_TEXT,
} ndef_type_t;

// 解析出的 NDEF 记录
typedef struct {
    ndef_type_t type;
    char        payload[512];  // URI 全文或 Text 内容
    char        lang[8];       // Text 记录的语言代码
} ndef_record_t;

// 从 NTAG dump 中解析第一条 NDEF 记录
esp_err_t ndef_parse(const ntag_dump_t* dump, ndef_record_t* out);

// 构建 NDEF URI 消息并写入 NTAG（page 4 开始）
// uri 为完整 URL（如 "https://example.com"），自动匹配前缀压缩
esp_err_t ndef_write_uri(const char* uri);

// 构建 NDEF Text 消息并写入 NTAG
// lang 如 "en"、"zh"；text 为 UTF-8 文本
esp_err_t ndef_write_text(const char* lang, const char* text);

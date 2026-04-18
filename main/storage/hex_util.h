#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// out 缓冲区至少 len*2 + 1 字节 (含结尾 \0)
void hex_encode_upper(const uint8_t* in, size_t len, char* out);
void hex_encode_lower(const uint8_t* in, size_t len, char* out);

bool hex_is_valid(const char* s, size_t expected_len);

// strict=false 时遇非 hex 字符以 0 填充,用于宽松解析上层数据
bool hex_decode(const char* s, uint8_t* out, size_t max_out, size_t* out_len, bool strict);

// 限制字符集防止 path traversal (作文件名 / URL 路径段)
bool id_char_ok(char c);
bool id_is_safe(const char* s, size_t max_len);

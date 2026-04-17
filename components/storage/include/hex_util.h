#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// hex 编/解码工具（小工具，避免在多个 api/存储模块重复实现）。

// 大写 hex：out 至少 len*2 + 1 字节。
void hex_encode_upper(const uint8_t* in, size_t len, char* out);
// 小写 hex：out 至少 len*2 + 1 字节。
void hex_encode_lower(const uint8_t* in, size_t len, char* out);

// 判断字符串是否全部为 hex 数字（0-9A-Fa-f）且长度 == expected_len。
bool hex_is_valid(const char* s, size_t expected_len);

// 解码 hex 到字节数组。strict=true 时任一非 hex 字符返回 false（不写 out）。
// 输出字节数 = strlen(s)/2，受 max_out 上限限制，写入 *out_len。
bool hex_decode(const char* s, uint8_t* out, size_t max_out, size_t* out_len, bool strict);

// 判断字符是否可用于文件名/URL ID（[0-9A-Za-z_-]）。
bool id_char_ok(char c);

// 判断整个 id 字符串是否安全（非空，长度 <= max_len，且字符全部 id_char_ok）。
bool id_is_safe(const char* s, size_t max_len);

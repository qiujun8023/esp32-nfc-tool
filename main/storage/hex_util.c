#include "hex_util.h"

#include <ctype.h>
#include <string.h>

static const char HEX_UPPER[] = "0123456789ABCDEF";
static const char HEX_LOWER[] = "0123456789abcdef";

static void hex_encode(const uint8_t* in, size_t len, char* out, const char* tab) {
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = tab[in[i] >> 4];
        out[i * 2 + 1] = tab[in[i] & 0xF];
    }
    out[len * 2] = 0;
}

void hex_encode_upper(const uint8_t* in, size_t len, char* out) {
    hex_encode(in, len, out, HEX_UPPER);
}

void hex_encode_lower(const uint8_t* in, size_t len, char* out) {
    hex_encode(in, len, out, HEX_LOWER);
}

bool hex_is_valid(const char* s, size_t expected_len) {
    if (!s || strlen(s) != expected_len) return false;
    for (size_t i = 0; i < expected_len; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode(const char* s, uint8_t* out, size_t max_out, size_t* out_len, bool strict) {
    if (!s || !out || !out_len) return false;
    size_t hl = strlen(s);
    if (strict && (hl & 1)) return false;
    size_t n = hl / 2;
    if (n > max_out) n = max_out;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(s[i * 2]);
        int lo = hex_nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            if (strict) return false;
            out[i] = 0;
        } else {
            out[i] = (uint8_t)((hi << 4) | lo);
        }
    }
    *out_len = n;
    return true;
}

bool id_char_ok(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '-';
}

bool id_is_safe(const char* s, size_t max_len) {
    if (!s || !*s) return false;
    size_t n = strnlen(s, max_len + 1);
    if (n == 0 || n > max_len) return false;
    for (size_t i = 0; i < n; i++) {
        if (!id_char_ok(s[i])) return false;
    }
    return true;
}

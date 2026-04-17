/**
 * @file ndef.c
 * @brief NDEF 消息解析与构建（NFC Forum Type 2 Tag）
 *
 * NTAG 内存布局：
 *   page 0-1: UID
 *   page 2:   lock bytes
 *   page 3:   CC (Capability Container)
 *   page 4+:  用户数据（NDEF TLV 从此开始）
 *
 * TLV 格式：
 *   0x00 = NULL TLV（跳过）
 *   0x03 = NDEF Message TLV，后接 1 或 3 字节长度
 *   0xFE = Terminator TLV
 *
 * NDEF Record（短格式）：
 *   flags(1) + TNF | type_len(1) | payload_len(1) | [id_len] | type | payload
 *   flags: MB=0x80, ME=0x40, CF=0x20, SR=0x10, IL=0x08, TNF=0x07
 */

#include "ndef.h"

#include <string.h>

#include "esp_log.h"

static const char* TAG = "ndef";

// URI 前缀表（索引 = 前缀代码）
static const char* URI_PREFIXES[] = {
    "",                    // 0x00
    "http://www.",         // 0x01
    "https://www.",        // 0x02
    "http://",             // 0x03
    "https://",            // 0x04
    "tel:",                // 0x05
    "mailto:",             // 0x06
};
#define URI_PREFIX_COUNT (sizeof(URI_PREFIXES) / sizeof(URI_PREFIXES[0]))

esp_err_t ndef_parse(const ntag_dump_t* dump, ndef_record_t* out) {
    memset(out, 0, sizeof(*out));

    // 用户数据从 page 4 开始，线性化为字节数组
    uint16_t max_bytes = (dump->total_pages - 4) * NTAG_PAGE_SIZE;
    const uint8_t* data = (const uint8_t*)&dump->pages[4];

    // 查找 NDEF Message TLV (0x03)
    uint16_t pos = 0;
    uint16_t ndef_len = 0;
    while (pos < max_bytes) {
        uint8_t t = data[pos++];
        if (t == 0x00) continue;        // NULL TLV
        if (t == 0xFE) return ESP_FAIL;  // Terminator，没找到 NDEF

        // 读长度
        uint16_t len;
        if (pos >= max_bytes) return ESP_FAIL;
        if (data[pos] == 0xFF) {
            // 3 字节长度格式
            if (pos + 2 >= max_bytes) return ESP_FAIL;
            len = ((uint16_t)data[pos + 1] << 8) | data[pos + 2];
            pos += 3;
        } else {
            len = data[pos++];
        }

        if (t == 0x03) {
            ndef_len = len;
            break;
        }
        pos += len;  // 跳过非 NDEF TLV
    }

    if (ndef_len == 0 || pos + ndef_len > max_bytes) return ESP_FAIL;

    // 解析 NDEF Record
    const uint8_t* rec = data + pos;
    if (ndef_len < 3) return ESP_FAIL;

    uint8_t flags    = rec[0];
    uint8_t tnf      = flags & 0x07;
    uint8_t type_len = rec[1];
    bool    sr       = (flags & 0x10) != 0;  // Short Record
    bool    il       = (flags & 0x08) != 0;  // ID Length present

    uint32_t payload_len;
    uint16_t hdr = 2;
    if (sr) {
        payload_len = rec[hdr++];
    } else {
        if (hdr + 3 >= ndef_len) return ESP_FAIL;
        payload_len = ((uint32_t)rec[hdr] << 24) | ((uint32_t)rec[hdr+1] << 16) |
                      ((uint32_t)rec[hdr+2] << 8) | rec[hdr+3];
        hdr += 4;
    }
    uint8_t id_len = 0;
    if (il) id_len = rec[hdr++];

    const uint8_t* type    = rec + hdr;
    const uint8_t* payload = type + type_len + id_len;

    if (payload + payload_len > rec + ndef_len) return ESP_FAIL;

    // TNF=0x01 (Well-Known), Type="U" → URI
    if (tnf == 0x01 && type_len == 1 && type[0] == 'U') {
        out->type = NDEF_TYPE_URI;
        uint8_t prefix_code = payload[0];
        const char* prefix = (prefix_code < URI_PREFIX_COUNT) ? URI_PREFIXES[prefix_code] : "";
        size_t plen = strlen(prefix);
        size_t rlen = payload_len - 1;
        if (plen + rlen >= sizeof(out->payload)) rlen = sizeof(out->payload) - plen - 1;
        memcpy(out->payload, prefix, plen);
        memcpy(out->payload + plen, payload + 1, rlen);
        out->payload[plen + rlen] = 0;
        return ESP_OK;
    }

    // TNF=0x01, Type="T" → Text
    if (tnf == 0x01 && type_len == 1 && type[0] == 'T') {
        out->type = NDEF_TYPE_TEXT;
        uint8_t status_byte = payload[0];
        uint8_t lang_len    = status_byte & 0x3F;
        if (lang_len > 7) lang_len = 7;
        memcpy(out->lang, payload + 1, lang_len);
        out->lang[lang_len] = 0;
        size_t text_len = payload_len - 1 - lang_len;
        if (text_len >= sizeof(out->payload)) text_len = sizeof(out->payload) - 1;
        memcpy(out->payload, payload + 1 + lang_len, text_len);
        out->payload[text_len] = 0;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "unsupported NDEF: TNF=%d type_len=%d", tnf, type_len);
    return ESP_ERR_NOT_SUPPORTED;
}

// 匹配最长 URI 前缀，返回前缀代码
static uint8_t match_uri_prefix(const char* uri, size_t* prefix_len) {
    uint8_t best = 0;
    size_t  best_len = 0;
    for (int i = 1; i < (int)URI_PREFIX_COUNT; i++) {
        size_t plen = strlen(URI_PREFIXES[i]);
        if (plen > best_len && strncmp(uri, URI_PREFIXES[i], plen) == 0) {
            best     = (uint8_t)i;
            best_len = plen;
        }
    }
    *prefix_len = best_len;
    return best;
}

// 写入 NDEF 消息到 NTAG（从 page 4 开始）
static esp_err_t write_ndef_message(const uint8_t* msg, size_t msg_len) {
    // 构建 TLV: 0x03 LEN MSG 0xFE
    uint8_t buf[4 + 3 + 512 + 1];  // CC + TLV header + message + terminator
    size_t pos = 0;

    // TLV header
    buf[pos++] = 0x03;  // NDEF Message TLV
    if (msg_len < 0xFF) {
        buf[pos++] = (uint8_t)msg_len;
    } else {
        buf[pos++] = 0xFF;
        buf[pos++] = (uint8_t)(msg_len >> 8);
        buf[pos++] = (uint8_t)(msg_len & 0xFF);
    }

    memcpy(buf + pos, msg, msg_len);
    pos += msg_len;
    buf[pos++] = 0xFE;  // Terminator

    // 按 4 字节一页写入（从 page 4 开始）
    for (size_t i = 0; i < pos; i += NTAG_PAGE_SIZE) {
        uint8_t page_data[NTAG_PAGE_SIZE] = {0};
        size_t remain = pos - i;
        if (remain > NTAG_PAGE_SIZE) remain = NTAG_PAGE_SIZE;
        memcpy(page_data, buf + i, remain);
        uint8_t page_num = 4 + (uint8_t)(i / NTAG_PAGE_SIZE);
        esp_err_t err = ntag_write_page(page_num, page_data);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write page %d failed", page_num);
            return err;
        }
    }

    ESP_LOGI(TAG, "NDEF message written, %d bytes", (int)pos);
    return ESP_OK;
}

esp_err_t ndef_write_uri(const char* uri) {
    if (!uri || !*uri) return ESP_ERR_INVALID_ARG;

    size_t prefix_len;
    uint8_t prefix_code = match_uri_prefix(uri, &prefix_len);
    const char* suffix = uri + prefix_len;
    size_t suffix_len = strlen(suffix);

    // NDEF Record: flags(1) + type_len(1) + payload_len(1) + type("U",1) + payload(1+suffix)
    size_t payload_len = 1 + suffix_len;  // prefix_code + suffix
    if (payload_len > 254) return ESP_ERR_INVALID_SIZE;

    uint8_t msg[260];
    size_t pos = 0;
    msg[pos++] = 0xD1;              // MB=1, ME=1, SR=1, TNF=Well-Known
    msg[pos++] = 0x01;              // Type Length = 1
    msg[pos++] = (uint8_t)payload_len;
    msg[pos++] = 'U';               // Type = URI
    msg[pos++] = prefix_code;
    memcpy(msg + pos, suffix, suffix_len);
    pos += suffix_len;

    return write_ndef_message(msg, pos);
}

esp_err_t ndef_write_text(const char* lang, const char* text) {
    if (!lang || !text) return ESP_ERR_INVALID_ARG;
    size_t lang_len = strlen(lang);
    size_t text_len = strlen(text);
    if (lang_len > 63) return ESP_ERR_INVALID_ARG;

    size_t payload_len = 1 + lang_len + text_len;  // status + lang + text
    if (payload_len > 254) return ESP_ERR_INVALID_SIZE;

    uint8_t msg[260];
    size_t pos = 0;
    msg[pos++] = 0xD1;              // MB=1, ME=1, SR=1, TNF=Well-Known
    msg[pos++] = 0x01;              // Type Length = 1
    msg[pos++] = (uint8_t)payload_len;
    msg[pos++] = 'T';               // Type = Text
    msg[pos++] = (uint8_t)lang_len;  // Status byte: UTF-8, lang_len
    memcpy(msg + pos, lang, lang_len);
    pos += lang_len;
    memcpy(msg + pos, text, text_len);
    pos += text_len;

    return write_ndef_message(msg, pos);
}

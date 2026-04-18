/*
 * NFC Forum Type 2 Tag (NTAG) NDEF 解析/构建。
 *
 * NTAG 内存布局:
 *   page 0-1: UID
 *   page 2:   lock bytes
 *   page 3:   CC (Capability Container)
 *   page 4+:  用户数据,NDEF TLV 从此开始
 *
 * TLV:
 *   0x00 = NULL TLV (跳过)
 *   0x03 = NDEF Message TLV,后接 1 或 3 字节长度
 *   0xFE = Terminator TLV
 *
 * NDEF Record 首字节 flags:
 *   MB=0x80, ME=0x40, CF=0x20, SR=0x10, IL=0x08, TNF=0x07 (低 3 位)
 */

#include "ndef.h"

#include <string.h>

#include "esp_log.h"

static const char* TAG = "ndef";

// NDEF URI record 前缀压缩表,数组下标 = 载荷首字节 prefix code (NFC Forum URI RTD 1.0)
static const char* URI_PREFIXES[] = {
    "",
    "http://www.",
    "https://www.",
    "http://",
    "https://",
    "tel:",
    "mailto:",
};
#define URI_PREFIX_COUNT (sizeof(URI_PREFIXES) / sizeof(URI_PREFIXES[0]))

esp_err_t ndef_parse(const ntag_dump_t* dump, ndef_record_t* out) {
    memset(out, 0, sizeof(*out));

    // 用户数据从 page 4 开始,按字节流扫 TLV
    uint16_t max_bytes = (dump->total_pages - 4) * NTAG_PAGE_SIZE;
    const uint8_t* data = (const uint8_t*)&dump->pages[4];

    uint16_t pos = 0;
    uint16_t ndef_len = 0;
    while (pos < max_bytes) {
        uint8_t t = data[pos++];
        if (t == 0x00) continue;
        if (t == 0xFE) return ESP_FAIL;

        uint16_t len;
        if (pos >= max_bytes) return ESP_FAIL;
        if (data[pos] == 0xFF) {
            // 长度字段 >= 255 时使用 3 字节扩展格式
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
        pos += len;
    }

    if (ndef_len == 0 || pos + ndef_len > max_bytes) return ESP_FAIL;

    const uint8_t* rec = data + pos;
    if (ndef_len < 3) return ESP_FAIL;

    uint8_t flags    = rec[0];
    uint8_t tnf      = flags & 0x07;
    uint8_t type_len = rec[1];
    bool    sr       = (flags & 0x10) != 0;  // Short Record
    bool    il       = (flags & 0x08) != 0;  // ID Length present

    uint32_t payload_len;
    uint32_t hdr = 2;
    if (sr) {
        if (hdr >= ndef_len) return ESP_FAIL;
        payload_len = rec[hdr++];
    } else {
        if (hdr + 4 > ndef_len) return ESP_FAIL;
        payload_len = ((uint32_t)rec[hdr] << 24) | ((uint32_t)rec[hdr + 1] << 16) |
                      ((uint32_t)rec[hdr + 2] << 8) | rec[hdr + 3];
        hdr += 4;
    }
    uint8_t id_len = 0;
    if (il) {
        if (hdr >= ndef_len) return ESP_FAIL;
        id_len = rec[hdr++];
    }

    // 每个 offset 都显式比 ndef_len,防止 type_len/payload_len 为恶意大值时指针越界
    if ((uint64_t)hdr + type_len + id_len + payload_len > ndef_len) return ESP_FAIL;

    const uint8_t* type    = rec + hdr;
    const uint8_t* payload = type + type_len + id_len;

    // TNF=0x01 (Well-Known) + Type="U": URI 记录
    if (tnf == 0x01 && type_len == 1 && type[0] == 'U') {
        if (payload_len < 1) return ESP_FAIL;
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

    // TNF=0x01 + Type="T": Text 记录
    if (tnf == 0x01 && type_len == 1 && type[0] == 'T') {
        if (payload_len < 1) return ESP_FAIL;
        out->type = NDEF_TYPE_TEXT;
        uint8_t status_byte = payload[0];
        // status byte 低 6 bit = 语言代码长度 (RFC 5646 语言 tag 通常 <= 7)
        uint8_t lang_len    = status_byte & 0x3F;
        if (lang_len > 7) lang_len = 7;
        if ((uint32_t)1 + lang_len > payload_len) return ESP_FAIL;
        memcpy(out->lang, payload + 1, lang_len);
        out->lang[lang_len] = 0;
        size_t text_len = payload_len - 1 - lang_len;
        if (text_len >= sizeof(out->payload)) text_len = sizeof(out->payload) - 1;
        memcpy(out->payload, payload + 1 + lang_len, text_len);
        out->payload[text_len] = 0;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "unsupported ndef: tnf=%d type_len=%d", tnf, type_len);
    return ESP_ERR_NOT_SUPPORTED;
}

// 贪婪匹配最长前缀,减少写入字节数(表里 https://www. 要比 https:// 优先)
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

// 各型号用户区最后一页(含),越过此页会进入 lock/config/PWD 区,写错可能永久锁卡
static uint8_t user_data_last_page(ntag_type_t t) {
    switch (t) {
        case NTAG_213: return 39;
        case NTAG_215: return 129;
        case NTAG_216: return 225;
        default:       return 15;  // Ultralight
    }
}

static esp_err_t write_ndef_message(const uint8_t* msg, size_t msg_len) {
    // 组 TLV: 0x03 LEN MSG 0xFE,缓冲上限按最大消息长度预留
    uint8_t buf[4 + 3 + 512 + 1];
    size_t pos = 0;

    buf[pos++] = 0x03;
    if (msg_len < 0xFF) {
        buf[pos++] = (uint8_t)msg_len;
    } else {
        // >= 255 时必须用 3 字节扩展长度,首字节为 0xFF 后跟大端 16 位
        buf[pos++] = 0xFF;
        buf[pos++] = (uint8_t)(msg_len >> 8);
        buf[pos++] = (uint8_t)(msg_len & 0xFF);
    }

    memcpy(buf + pos, msg, msg_len);
    pos += msg_len;
    buf[pos++] = 0xFE;

    // 先检卡容量,避免越界写 lock/config 页导致永久损坏
    ntag_type_t type = ntag_detect_type();
    uint8_t last_page = user_data_last_page(type);
    size_t max_bytes = (size_t)(last_page - 4 + 1) * NTAG_PAGE_SIZE;
    if (pos > max_bytes) {
        ESP_LOGE(TAG, "ndef %u bytes exceeds card capacity %u", (unsigned)pos, (unsigned)max_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

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
    // 0xD1 = MB | ME | SR | TNF(Well-Known)
    msg[pos++] = 0xD1;
    msg[pos++] = 0x01;
    msg[pos++] = (uint8_t)payload_len;
    msg[pos++] = 'U';
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
    // 0xD1 = MB | ME | SR | TNF(Well-Known)
    msg[pos++] = 0xD1;
    msg[pos++] = 0x01;
    msg[pos++] = (uint8_t)payload_len;
    msg[pos++] = 'T';
    // status byte: bit7=0 (UTF-8), bit0-5 = 语言代码长度
    msg[pos++] = (uint8_t)lang_len;
    memcpy(msg + pos, lang, lang_len);
    pos += lang_len;
    memcpy(msg + pos, text, text_len);
    pos += text_len;

    return write_ndef_message(msg, pos);
}

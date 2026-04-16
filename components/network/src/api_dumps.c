/**
 * @file api_dumps.c
 * @brief dump 卡库相关 API
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "dump_store.h"
#include "esp_log.h"
#include "http_server.h"
#include "mifare_classic.h"
#include "ntag.h"
#include "pn532.h"

static const char* TAG = "api_dumps";

static const char* extract_id(httpd_req_t* req, char* out, size_t outlen) {
    // URI 形如 /api/dumps/<id> 或 /api/dumps/<id>/bin
    const char* prefix = "/api/dumps/";
    const char* p      = strstr(req->uri, prefix);
    if (!p) return NULL;
    p += strlen(prefix);
    size_t i = 0;
    while (*p && *p != '/' && *p != '?' && i < outlen - 1) out[i++] = *p++;
    out[i] = 0;
    return out;
}

static esp_err_t handle_list(httpd_req_t* req) {
    static dump_meta_t metas[64];
    size_t             n = dump_store_list(metas, 64);

    cJSON* arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", metas[i].id);
        cJSON_AddStringToObject(o, "name", metas[i].name);
        cJSON_AddNumberToObject(o, "type", metas[i].type);
        char hex[24];
        static const char H[] = "0123456789ABCDEF";
        for (uint8_t j = 0; j < metas[i].uid_len; j++) {
            hex[j * 2]     = H[metas[i].uid[j] >> 4];
            hex[j * 2 + 1] = H[metas[i].uid[j] & 0xF];
        }
        hex[metas[i].uid_len * 2] = 0;
        cJSON_AddStringToObject(o, "uid", hex);
        cJSON_AddNumberToObject(o, "blocks", metas[i].block_or_page_count);
        cJSON_AddNumberToObject(o, "knownKeys", metas[i].known_keys);
        cJSON_AddNumberToObject(o, "seq", metas[i].seq);
        cJSON_AddNumberToObject(o, "binSize", metas[i].bin_size);
        cJSON_AddItemToArray(arr, o);
    }
    char* s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

static esp_err_t handle_download(httpd_req_t* req) {
    char id[40];
    if (!extract_id(req, id, sizeof(id))) return httpd_resp_send_404(req);

    uint8_t* buf  = NULL;
    size_t   blen = 0;
    if (dump_store_read_bin(id, &buf, &blen) != ESP_OK) return httpd_resp_send_404(req);

    httpd_resp_set_type(req, "application/octet-stream");
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s.bin\"", id);
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);
    httpd_resp_send(req, (const char*)buf, blen);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_download_txt(httpd_req_t* req) {
    char id[40];
    if (!extract_id(req, id, sizeof(id))) return httpd_resp_send_404(req);

    uint8_t* buf  = NULL;
    size_t   blen = 0;
    if (dump_store_read_bin(id, &buf, &blen) != ESP_OK) return httpd_resp_send_404(req);

    // 转为 hex dump：每行 16 字节 = 32 hex 字符 + 换行
    size_t txt_len = (blen / 16) * 33 + ((blen % 16) ? (blen % 16) * 2 + 1 : 0);
    char* txt = malloc(txt_len + 1);
    if (!txt) {
        free(buf);
        return httpd_resp_send_500(req);
    }
    static const char H[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; i < blen; i++) {
        txt[pos++] = H[buf[i] >> 4];
        txt[pos++] = H[buf[i] & 0xF];
        if ((i + 1) % 16 == 0 || i == blen - 1) txt[pos++] = '\n';
    }
    txt[pos] = 0;
    free(buf);

    httpd_resp_set_type(req, "text/plain");
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s.txt\"", id);
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);
    httpd_resp_send(req, txt, pos);
    free(txt);
    return ESP_OK;
}

static esp_err_t handle_upload(httpd_req_t* req) {
    // 接收上传的 .bin/.mfd 文件，存入 LittleFS
    size_t total = req->content_len;
    if (total == 0 || total > 4096) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"文件大小无效 (1-4096 字节)\"}");
    }

    uint8_t* buf = malloc(total);
    if (!buf) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"内存不足\"}");
    }

    size_t received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, (char*)buf + received, total - received);
        if (n <= 0) {
            free(buf);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"接收失败\"}");
        }
        received += n;
    }

    // 从 query 参数获取 name 和 type
    char name[64] = "导入";
    char type_str[8] = "mfc";
    size_t qlen;
    qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 256) {
        char query[256];
        httpd_req_get_url_query_str(req, query, sizeof(query));
        httpd_query_key_value(query, "name", name, sizeof(name));
        httpd_query_key_value(query, "type", type_str, sizeof(type_str));
    }

    // 根据大小和类型判断
    dump_type_t dtype;
    if (strcmp(type_str, "ntag") == 0) {
        dtype = DUMP_TYPE_NTAG;
    } else {
        dtype = DUMP_TYPE_MIFARE_CLASSIC;
    }

    char id[40];
    esp_err_t err;

    if (dtype == DUMP_TYPE_MIFARE_CLASSIC) {
        mfc_dump_t* dump = calloc(1, sizeof(mfc_dump_t));
        if (!dump) { free(buf); return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"内存不足\"}"); }
        // 从 bin 重建 dump 结构
        dump->block_count = (total >= 4096) ? 256 : 64;
        dump->sector_count = (dump->block_count > 64) ? 40 : 16;
        dump->target.type = (dump->block_count > 64) ? PN532_CARD_MIFARE_CLASSIC_4K : PN532_CARD_MIFARE_CLASSIC_1K;
        dump->target.sak = (dump->block_count > 64) ? 0x18 : 0x08;
        // UID 从 block 0 前 4 字节
        if (total >= 16) {
            memcpy(dump->target.uid, buf, 4);
            dump->target.uid_len = 4;
        }
        size_t copy = total > sizeof(dump->data) ? sizeof(dump->data) : total;
        memcpy(dump->data, buf, copy);
        for (uint16_t b = 0; b < dump->block_count; b++) dump->block_read[b] = true;
        // 从 sector trailer 提取 key
        for (uint8_t s = 0; s < dump->sector_count; s++) {
            uint8_t tb = mfc_sector_first_block(s) + mfc_sector_block_count(s) - 1;
            uint8_t* t = dump->data[tb];
            bool a_zero = true, b_zero = true;
            for (int k = 0; k < 6; k++) { if (t[k]) a_zero = false; if (t[10+k]) b_zero = false; }
            if (!a_zero) { dump->key_a[s].found = true; memcpy(dump->key_a[s].key, t, 6); }
            if (!b_zero) { dump->key_b[s].found = true; memcpy(dump->key_b[s].key, t+10, 6); }
        }
        err = dump_store_save_mfc(dump, name, id);
        free(dump);
    } else {
        ntag_dump_t* dump = calloc(1, sizeof(ntag_dump_t));
        if (!dump) { free(buf); return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"内存不足\"}"); }
        dump->total_pages = total / NTAG_PAGE_SIZE;
        size_t copy = total > sizeof(dump->pages) ? sizeof(dump->pages) : total;
        memcpy(dump->pages, buf, copy);
        for (uint16_t p = 0; p < dump->total_pages; p++) dump->page_read[p] = true;
        // 需要一个 target 信息
        pn532_target_t tgt = {0};
        if (total >= 7) { memcpy(tgt.uid, buf, 7); tgt.uid_len = 7; }
        err = dump_store_save_ntag(dump, &tgt, name, id);
        free(dump);
    }
    free(buf);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        char resp[80];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"id\":\"%s\"}", id);
        return httpd_resp_sendstr(req, resp);
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"保存失败\"}");
}

/**
 * @brief 解码 access bits (byte 6,7,8 of sector trailer)
 *
 * Access bits 存储在 trailer 的 byte 6/7/8：
 *   byte6 = ~C2_b1~C2_b0~C1_b1~C1_b0~C0_b1~C0_b0 (inverted bits in high nibble)
 *   byte7 = C1_b3C1_b2C1_b1C1_b0~C3_b1~C3_b0~C2_b3~C2_b2
 *   byte8 = C3_b3C3_b2C3_b1C3_b0C2_b3C2_b2C2_b1C2_b0 (wait... let me use standard notation)
 *
 * Standard: 3 bits per block (C1, C2, C3), 4 blocks per sector
 *   byte 6: !C2_3 !C2_2 !C2_1 !C2_0  !C1_3 !C1_2 !C1_1 !C1_0
 *   byte 7:  C1_3  C1_2  C1_1  C1_0  !C3_3 !C3_2 !C3_1 !C3_0
 *   byte 8:  C3_3  C3_2  C3_1  C3_0   C2_3  C2_2  C2_1  C2_0
 */
static void decode_access_bits(uint8_t b6, uint8_t b7, uint8_t b8, uint8_t c[4]) {
    // Extract C1, C2, C3 for each block (0-3)
    for (int i = 0; i < 4; i++) {
        uint8_t c1 = (b7 >> (4 + i)) & 1;
        uint8_t c2 = (b8 >> i) & 1;
        uint8_t c3 = (b8 >> (4 + i)) & 1;
        c[i] = (c3 << 2) | (c2 << 1) | c1;
    }
}

static const char* access_desc_data(uint8_t c) {
    switch (c) {
        case 0: return "KeyA|B 读写";
        case 1: return "KeyA|B 读, KeyB 写";
        case 2: return "KeyA|B 读, 不可写";
        case 3: return "KeyB 读写";
        case 4: return "KeyA|B 读, KeyB 写";
        case 5: return "不可读写";
        case 6: return "KeyB 读, KeyB 写";
        case 7: return "不可读写";
        default: return "未知";
    }
}

static const char* access_desc_trailer(uint8_t c) {
    switch (c) {
        case 0: return "KeyA 不可读, KeyA 写 | ACC: KeyA 读, 不可写 | KeyB: KeyA 读写";
        case 1: return "KeyA 不可读 | ACC: KeyA 读 | KeyB: KeyA 读, 不可写";
        case 3: return "KeyA 不可读 | ACC: KeyA|B 读 | KeyB: 不可读写";
        case 4: return "KeyA 不可读, KeyB 写 | ACC: KeyA|B 读 | KeyB: 不可读写";
        default: return "参见 MIFARE 规格";
    }
}

static esp_err_t handle_detail(httpd_req_t* req) {
    char id[40];
    if (!extract_id(req, id, sizeof(id))) return httpd_resp_send_404(req);

    dump_meta_t meta;
    if (dump_store_get_meta(id, &meta) != ESP_OK) return httpd_resp_send_404(req);
    if (meta.type != DUMP_TYPE_MIFARE_CLASSIC) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"仅支持 MFC\"}");
    }

    uint8_t* bin = NULL;
    size_t blen;
    if (dump_store_read_bin(id, &bin, &blen) != ESP_OK) return httpd_resp_send_404(req);

    uint8_t sector_count = (meta.block_or_page_count > 64) ? 40 : 16;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* sectors = cJSON_CreateArray();

    for (uint8_t s = 0; s < sector_count; s++) {
        uint8_t first = mfc_sector_first_block(s);
        uint8_t blocks = mfc_sector_block_count(s);
        uint8_t trailer_blk = first + blocks - 1;

        if ((size_t)(trailer_blk + 1) * 16 > blen) break;

        uint8_t* trailer = bin + trailer_blk * 16;
        uint8_t c[4];
        decode_access_bits(trailer[6], trailer[7], trailer[8], c);

        cJSON* sec = cJSON_CreateObject();
        cJSON_AddNumberToObject(sec, "sector", s);

        // Key A (hex, 如果非全零)
        char ka[13] = {0}, kb[13] = {0};
        static const char H[] = "0123456789ABCDEF";
        bool ka_zero = true, kb_zero = true;
        for (int i = 0; i < 6; i++) {
            ka[i*2] = H[trailer[i]>>4]; ka[i*2+1] = H[trailer[i]&0xF];
            kb[i*2] = H[trailer[10+i]>>4]; kb[i*2+1] = H[trailer[10+i]&0xF];
            if (trailer[i]) ka_zero = false;
            if (trailer[10+i]) kb_zero = false;
        }
        if (!ka_zero) cJSON_AddStringToObject(sec, "keyA", ka);
        if (!kb_zero) cJSON_AddStringToObject(sec, "keyB", kb);

        // 每个 block 的权限
        cJSON* blks = cJSON_CreateArray();
        for (uint8_t b = 0; b < blocks; b++) {
            cJSON* blk = cJSON_CreateObject();
            cJSON_AddNumberToObject(blk, "block", first + b);
            cJSON_AddNumberToObject(blk, "access", c[b < 3 ? b : 3]);
            if (b == blocks - 1) {
                cJSON_AddStringToObject(blk, "desc", access_desc_trailer(c[3]));
                cJSON_AddStringToObject(blk, "role", "trailer");
            } else {
                cJSON_AddStringToObject(blk, "desc", access_desc_data(c[b]));
                cJSON_AddStringToObject(blk, "role", "data");
            }
            cJSON_AddItemToArray(blks, blk);
        }
        cJSON_AddItemToObject(sec, "blocks", blks);
        cJSON_AddItemToArray(sectors, sec);
    }
    cJSON_AddItemToObject(root, "sectors", sectors);
    free(bin);

    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

static esp_err_t handle_delete(httpd_req_t* req) {
    char id[40];
    if (!extract_id(req, id, sizeof(id))) return httpd_resp_send_404(req);
    dump_store_delete(id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_rename(httpd_req_t* req) {
    char id[40];
    if (!extract_id(req, id, sizeof(id))) return httpd_resp_send_404(req);
    char body[128] = {0};
    int  n         = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return httpd_resp_sendstr(req, "{\"ok\":false}");
    body[n]   = 0;
    cJSON* j = cJSON_Parse(body);
    if (!j) return httpd_resp_sendstr(req, "{\"ok\":false}");
    cJSON* nm = cJSON_GetObjectItem(j, "name");
    if (cJSON_IsString(nm)) {
        dump_store_rename(id, nm->valuestring);
    }
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void api_dumps_register(httpd_handle_t srv) {
    httpd_uri_t u_list   = {.uri = "/api/dumps", .method = HTTP_GET, .handler = handle_list};
    httpd_uri_t u_upload = {.uri = "/api/dumps/upload", .method = HTTP_POST, .handler = handle_upload};
    httpd_uri_t u_dl     = {.uri = "/api/dumps/*/bin", .method = HTTP_GET, .handler = handle_download};
    httpd_uri_t u_dl_txt = {.uri = "/api/dumps/*/txt", .method = HTTP_GET, .handler = handle_download_txt};
    httpd_uri_t u_detail = {.uri = "/api/dumps/*/detail", .method = HTTP_GET, .handler = handle_detail};
    httpd_uri_t u_del    = {.uri = "/api/dumps/*", .method = HTTP_DELETE, .handler = handle_delete};
    httpd_uri_t u_rename = {.uri = "/api/dumps/*", .method = HTTP_PATCH, .handler = handle_rename};

    httpd_register_uri_handler(srv, &u_list);
    httpd_register_uri_handler(srv, &u_upload);
    httpd_register_uri_handler(srv, &u_dl);
    httpd_register_uri_handler(srv, &u_dl_txt);
    httpd_register_uri_handler(srv, &u_detail);
    httpd_register_uri_handler(srv, &u_del);
    httpd_register_uri_handler(srv, &u_rename);
    ESP_LOGI(TAG, "Dumps API registered");
}

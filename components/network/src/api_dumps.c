/**
 * @file api_dumps.c
 * @brief dump 卡库相关 API
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dump_store.h"
#include "esp_log.h"
#include "hex_util.h"
#include "http_server.h"
#include "mifare_classic.h"
#include "ndef.h"
#include "ntag.h"
#include "pn532.h"

static const char* TAG = "api_dumps";

#define DUMP_ID_MAX_LEN 39  // 不含结尾 \0
#define DUMP_LIST_MAX   128
#define DUMP_UPLOAD_MAX 4096

static const char JSON_CT[] = "application/json";

// 统一的 JSON 错误响应。
static esp_err_t send_err(httpd_req_t* req, const char* err) {
    httpd_resp_set_type(req, JSON_CT);
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\"}", err ? err : "failed");
    return httpd_resp_sendstr(req, buf);
}

/*
 * 从 URI 提取 dump id（prefix 之后、下一个 / 或 ? 之前）。
 *
 * ESP-IDF httpd_uri_match_wildcard 只支持模板尾部的 '*'，所以必须把动作放在 id
 * 之前：/api/dumps/detail/<id>、/api/dumps/bin/<id>；DELETE/PATCH 继续用
 * /api/dumps/<id>（trailing '*'）。各 handler 传入各自的 prefix。
 */
static bool extract_id_after(httpd_req_t* req, const char* prefix, char* out, size_t outlen) {
    const char* p = strstr(req->uri, prefix);
    if (!p) return false;
    p += strlen(prefix);
    size_t i = 0;
    while (*p && *p != '/' && *p != '?' && i < outlen - 1) out[i++] = *p++;
    out[i] = 0;
    return id_is_safe(out, outlen - 1);
}

static esp_err_t handle_list(httpd_req_t* req) {
    dump_meta_t* metas = calloc(DUMP_LIST_MAX, sizeof(dump_meta_t));
    if (!metas) {
        httpd_resp_set_type(req, JSON_CT);
        return httpd_resp_sendstr(req, "[]");
    }
    size_t n = dump_store_list(metas, DUMP_LIST_MAX);

    cJSON* arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", metas[i].id);
        cJSON_AddStringToObject(o, "name", metas[i].name);
        cJSON_AddNumberToObject(o, "type", metas[i].type);
        char hex[24];
        hex_encode_upper(metas[i].uid, metas[i].uid_len, hex);
        cJSON_AddStringToObject(o, "uid", hex);
        cJSON_AddNumberToObject(o, "blocks", metas[i].block_or_page_count);
        cJSON_AddNumberToObject(o, "knownKeys", metas[i].known_keys);
        cJSON_AddNumberToObject(o, "seq", metas[i].seq);
        cJSON_AddNumberToObject(o, "binSize", metas[i].bin_size);
        if (metas[i].created_ms > 0) {
            cJSON_AddNumberToObject(o, "createdMs", (double)metas[i].created_ms);
        }
        cJSON_AddItemToArray(arr, o);
    }
    free(metas);
    char* s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, JSON_CT);
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

static esp_err_t handle_download(httpd_req_t* req) {
    char id[40];
    if (!extract_id_after(req, "/api/dumps/bin/", id, sizeof(id))) return httpd_resp_send_404(req);

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

// 严格判定 MFC 卡型：1024=1K，4096=4K；其他尺寸不接受。
static bool infer_mfc_size(size_t total, uint16_t* block_count, uint8_t* sector_count,
                           pn532_card_type_t* type, uint8_t* sak) {
    if (total == 1024) {
        *block_count  = 64;
        *sector_count = MFC_1K_SECTORS;
        *type         = PN532_CARD_MIFARE_CLASSIC_1K;
        *sak          = 0x08;
        return true;
    }
    if (total == 4096) {
        *block_count  = 256;
        *sector_count = MFC_4K_SECTORS;
        *type         = PN532_CARD_MIFARE_CLASSIC_4K;
        *sak          = 0x18;
        return true;
    }
    return false;
}

static esp_err_t rebuild_and_save_mfc(const uint8_t* buf, size_t total,
                                      const char* name, int64_t created_ms,
                                      char id_out[40]) {
    uint16_t           block_count;
    uint8_t            sector_count;
    pn532_card_type_t  type;
    uint8_t            sak;
    if (!infer_mfc_size(total, &block_count, &sector_count, &type, &sak)) {
        return ESP_ERR_INVALID_ARG;
    }

    mfc_dump_t* dump = calloc(1, sizeof(mfc_dump_t));
    if (!dump) return ESP_ERR_NO_MEM;

    dump->block_count  = block_count;
    dump->sector_count = sector_count;
    dump->target.type  = type;
    dump->target.sak   = sak;
    // 从 block 0 提取 UID（前 4 字节，单尺寸 UID）
    memcpy(dump->target.uid, buf, 4);
    dump->target.uid_len = 4;
    memcpy(dump->data, buf, total);

    dump_store_recover_trailer_keys(dump);

    esp_err_t err = dump_store_save_mfc(dump, name, created_ms, id_out);
    free(dump);
    return err;
}

static esp_err_t rebuild_and_save_ntag(const uint8_t* buf, size_t total,
                                       const char* name, int64_t created_ms,
                                       char id_out[40]) {
    if (total == 0 || total % NTAG_PAGE_SIZE != 0) return ESP_ERR_INVALID_ARG;
    ntag_dump_t* dump = calloc(1, sizeof(ntag_dump_t));
    if (!dump) return ESP_ERR_NO_MEM;

    dump->total_pages = total / NTAG_PAGE_SIZE;
    if (dump->total_pages > (sizeof(dump->pages) / NTAG_PAGE_SIZE)) {
        dump->total_pages = sizeof(dump->pages) / NTAG_PAGE_SIZE;
    }
    size_t copy = (size_t)dump->total_pages * NTAG_PAGE_SIZE;
    memcpy(dump->pages, buf, copy);
    for (uint16_t p = 0; p < dump->total_pages; p++) dump->page_read[p] = true;

    pn532_target_t tgt = {0};
    if (copy >= 7) {
        memcpy(tgt.uid, buf, 7);
        tgt.uid_len = 7;
    }

    esp_err_t err = dump_store_save_ntag(dump, &tgt, name, created_ms, id_out);
    free(dump);
    return err;
}

static esp_err_t handle_upload(httpd_req_t* req) {
    size_t total = req->content_len;
    if (total == 0 || total > DUMP_UPLOAD_MAX) {
        return send_err(req, "文件大小无效 (1-4096 字节)");
    }

    uint8_t* buf = malloc(total);
    if (!buf) return send_err(req, "内存不足");

    size_t received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, (char*)buf + received, total - received);
        if (n <= 0) {
            free(buf);
            return send_err(req, "接收失败");
        }
        received += n;
    }

    char name[64]     = "导入";
    char type_str[8]  = "mfc";
    int64_t created_ms = 0;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 256) {
        char query[256];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            httpd_query_key_value(query, "name", name, sizeof(name));
            httpd_query_key_value(query, "type", type_str, sizeof(type_str));
            char ts_str[24] = {0};
            if (httpd_query_key_value(query, "ts", ts_str, sizeof(ts_str)) == ESP_OK) {
                created_ms = (int64_t)strtoll(ts_str, NULL, 10);
            }
        }
    }

    char      id[40] = {0};
    esp_err_t err;
    if (strcmp(type_str, "ntag") == 0) {
        err = rebuild_and_save_ntag(buf, total, name, created_ms, id);
    } else {
        err = rebuild_and_save_mfc(buf, total, name, created_ms, id);
    }
    free(buf);

    if (err == ESP_OK) {
        httpd_resp_set_type(req, JSON_CT);
        char resp[80];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"id\":\"%s\"}", id);
        return httpd_resp_sendstr(req, resp);
    }
    if (err == ESP_ERR_INVALID_ARG) return send_err(req, "文件格式与卡型不匹配");
    if (err == ESP_ERR_NO_MEM)      return send_err(req, "内存不足");
    return send_err(req, "保存失败");
}

/**
 * @brief 解码 access bits (byte 6,7,8 of sector trailer)
 *
 * 3 bits per block (C1, C2, C3), 4 blocks per sector
 *   byte 6: !C2_3 !C2_2 !C2_1 !C2_0  !C1_3 !C1_2 !C1_1 !C1_0
 *   byte 7:  C1_3  C1_2  C1_1  C1_0  !C3_3 !C3_2 !C3_1 !C3_0
 *   byte 8:  C3_3  C3_2  C3_1  C3_0   C2_3  C2_2  C2_1  C2_0
 */
static void decode_access_bits(uint8_t b6, uint8_t b7, uint8_t b8, uint8_t c[4]) {
    (void)b6;
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

static esp_err_t send_detail_json(httpd_req_t* req, cJSON* root) {
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, JSON_CT);
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

static ntag_type_t ntag_type_from_pages(uint16_t pages) {
    if (pages >= 231) return NTAG_216;
    if (pages >= 135) return NTAG_215;
    if (pages >= 45)  return NTAG_213;
    return NTAG_UNKNOWN;
}

static esp_err_t handle_ntag_detail(httpd_req_t* req, const dump_meta_t* meta, const uint8_t* bin, size_t blen) {
    uint16_t total_pages = (uint16_t)(blen / NTAG_PAGE_SIZE);
    if (total_pages < 4) return send_err(req, "NTAG 数据不完整");

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "kind", "ntag");

    ntag_type_t t = ntag_type_from_pages(total_pages);
    cJSON_AddNumberToObject(root, "ntagType", (int)t);
    cJSON_AddNumberToObject(root, "totalPages", total_pages);
    cJSON_AddNumberToObject(root, "userBytes", (total_pages >= 4) ? (total_pages - 4) * NTAG_PAGE_SIZE : 0);

    char uid_hex[24];
    hex_encode_upper(meta->uid, meta->uid_len, uid_hex);
    cJSON_AddStringToObject(root, "uid", uid_hex);

    /* 静态锁字节：page 2 byte 2-3 */
    char lock_hex[8];
    hex_encode_upper(bin + 2 * NTAG_PAGE_SIZE + 2, 2, lock_hex);
    cJSON_AddStringToObject(root, "lock", lock_hex);

    /* Capability Container: page 3 */
    const uint8_t* cc_p = bin + 3 * NTAG_PAGE_SIZE;
    cJSON* cc = cJSON_CreateObject();
    char byte_hex[4];
    hex_encode_upper(cc_p, 1, byte_hex); cJSON_AddStringToObject(cc, "magic", byte_hex);
    hex_encode_upper(cc_p + 1, 1, byte_hex); cJSON_AddStringToObject(cc, "version", byte_hex);
    hex_encode_upper(cc_p + 3, 1, byte_hex); cJSON_AddStringToObject(cc, "access", byte_hex);
    cJSON_AddBoolToObject(cc, "magicOk", cc_p[0] == 0xE1);
    cJSON_AddNumberToObject(cc, "sizeBytes", cc_p[2] * 8);
    cJSON_AddItemToObject(root, "cc", cc);

    /* NDEF 解析 */
    ntag_dump_t* nd = calloc(1, sizeof(ntag_dump_t));
    if (nd) {
        nd->total_pages = total_pages;
        memcpy(nd->pages, bin, (size_t)total_pages * NTAG_PAGE_SIZE);
        ndef_record_t rec;
        cJSON* ndef = cJSON_CreateObject();
        if (ndef_parse(nd, &rec) == ESP_OK) {
            cJSON_AddBoolToObject(ndef, "found", true);
            if (rec.type == NDEF_TYPE_URI) {
                cJSON_AddStringToObject(ndef, "type", "uri");
                cJSON_AddStringToObject(ndef, "payload", rec.payload);
            } else if (rec.type == NDEF_TYPE_TEXT) {
                cJSON_AddStringToObject(ndef, "type", "text");
                cJSON_AddStringToObject(ndef, "lang", rec.lang);
                cJSON_AddStringToObject(ndef, "payload", rec.payload);
            } else {
                cJSON_AddStringToObject(ndef, "type", "unknown");
            }
        } else {
            cJSON_AddBoolToObject(ndef, "found", false);
        }
        cJSON_AddItemToObject(root, "ndef", ndef);
        free(nd);
    }

    /* 逐页 hex */
    cJSON* pages = cJSON_CreateArray();
    char page_hex[NTAG_PAGE_SIZE * 2 + 1];
    for (uint16_t p = 0; p < total_pages; p++) {
        hex_encode_upper(bin + p * NTAG_PAGE_SIZE, NTAG_PAGE_SIZE, page_hex);
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "page", p);
        cJSON_AddStringToObject(o, "hex", page_hex);
        cJSON_AddItemToArray(pages, o);
    }
    cJSON_AddItemToObject(root, "pages", pages);

    return send_detail_json(req, root);
}

static esp_err_t handle_mfc_detail(httpd_req_t* req, const dump_meta_t* meta, const uint8_t* bin, size_t blen) {
    uint8_t sector_count = (meta->block_or_page_count > 64) ? MFC_4K_SECTORS : MFC_1K_SECTORS;
    cJSON*  root         = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "kind", "mfc");
    cJSON* sectors = cJSON_CreateArray();

    for (uint8_t s = 0; s < sector_count; s++) {
        uint8_t first       = mfc_sector_first_block(s);
        uint8_t blocks      = mfc_sector_block_count(s);
        uint8_t trailer_blk = first + blocks - 1;

        if ((size_t)(trailer_blk + 1) * 16 > blen) break;

        const uint8_t* trailer = bin + trailer_blk * 16;
        uint8_t  c[4];
        decode_access_bits(trailer[6], trailer[7], trailer[8], c);

        cJSON* sec = cJSON_CreateObject();
        cJSON_AddNumberToObject(sec, "sector", s);

        bool ka_zero = true, kb_zero = true;
        for (int i = 0; i < 6; i++) {
            if (trailer[i])      ka_zero = false;
            if (trailer[10 + i]) kb_zero = false;
        }
        char ka[13], kb[13];
        hex_encode_upper(trailer,       6, ka);
        hex_encode_upper(trailer + 10,  6, kb);
        if (!ka_zero) cJSON_AddStringToObject(sec, "keyA", ka);
        if (!kb_zero) cJSON_AddStringToObject(sec, "keyB", kb);

        // 4-block 扇区: c[0]=block0, c[1]=block1, c[2]=block2, c[3]=trailer
        // 16-block 扇区: c[0]=block0-4, c[1]=block5-9, c[2]=block10-14, c[3]=trailer(block15)
        cJSON* blks = cJSON_CreateArray();
        for (uint8_t b = 0; b < blocks; b++) {
            cJSON* blk = cJSON_CreateObject();
            cJSON_AddNumberToObject(blk, "block", first + b);
            uint8_t ci;
            if (b == blocks - 1) {
                ci = 3;
            } else if (blocks == 4) {
                ci = b;
            } else {
                ci = b / 5;
                if (ci > 2) ci = 2;
            }
            cJSON_AddNumberToObject(blk, "access", c[ci]);
            if (b == blocks - 1) {
                cJSON_AddStringToObject(blk, "desc", access_desc_trailer(c[3]));
                cJSON_AddStringToObject(blk, "role", "trailer");
            } else {
                cJSON_AddStringToObject(blk, "desc", access_desc_data(c[ci]));
                cJSON_AddStringToObject(blk, "role", "data");
            }
            cJSON_AddItemToArray(blks, blk);
        }
        cJSON_AddItemToObject(sec, "blocks", blks);
        cJSON_AddItemToArray(sectors, sec);
    }
    cJSON_AddItemToObject(root, "sectors", sectors);
    return send_detail_json(req, root);
}

static esp_err_t handle_detail(httpd_req_t* req) {
    char id[40];
    if (!extract_id_after(req, "/api/dumps/detail/", id, sizeof(id))) {
        return send_err(req, "非法 id");
    }

    dump_meta_t meta;
    if (dump_store_get_meta(id, &meta) != ESP_OK) {
        return send_err(req, "meta 不存在");
    }

    uint8_t* bin = NULL;
    size_t   blen = 0;
    if (dump_store_read_bin(id, &bin, &blen) != ESP_OK) {
        return send_err(req, "bin 读取失败");
    }

    esp_err_t err;
    if (meta.type == DUMP_TYPE_MIFARE_CLASSIC) {
        err = handle_mfc_detail(req, &meta, bin, blen);
    } else if (meta.type == DUMP_TYPE_NTAG) {
        err = handle_ntag_detail(req, &meta, bin, blen);
    } else {
        err = send_err(req, "未知类型");
    }
    free(bin);
    return err;
}

static esp_err_t handle_delete(httpd_req_t* req) {
    char id[40];
    if (!extract_id_after(req, "/api/dumps/", id, sizeof(id))) return httpd_resp_send_404(req);
    dump_store_delete(id);
    httpd_resp_set_type(req, JSON_CT);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t handle_rename(httpd_req_t* req) {
    char id[40];
    if (!extract_id_after(req, "/api/dumps/", id, sizeof(id))) return httpd_resp_send_404(req);
    char body[128] = {0};
    int  n         = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return send_err(req, "空 body");
    body[n] = 0;
    cJSON* j = cJSON_Parse(body);
    if (!j) return send_err(req, "JSON 错");
    cJSON* nm = cJSON_GetObjectItem(j, "name");
    esp_err_t err = ESP_FAIL;
    if (cJSON_IsString(nm)) {
        err = dump_store_rename(id, nm->valuestring);
    }
    cJSON_Delete(j);
    httpd_resp_set_type(req, JSON_CT);
    return httpd_resp_sendstr(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
}

void api_dumps_register(httpd_handle_t srv) {
    httpd_uri_t u_list   = {.uri = "/api/dumps", .method = HTTP_GET, .handler = handle_list};
    httpd_uri_t u_upload = {.uri = "/api/dumps/upload", .method = HTTP_POST, .handler = handle_upload};
    httpd_uri_t u_dl     = {.uri = "/api/dumps/bin/*", .method = HTTP_GET, .handler = handle_download};
    httpd_uri_t u_detail = {.uri = "/api/dumps/detail/*", .method = HTTP_GET, .handler = handle_detail};
    httpd_uri_t u_del    = {.uri = "/api/dumps/*", .method = HTTP_DELETE, .handler = handle_delete};
    httpd_uri_t u_rename = {.uri = "/api/dumps/*", .method = HTTP_PATCH, .handler = handle_rename};

    httpd_register_uri_handler(srv, &u_list);
    httpd_register_uri_handler(srv, &u_upload);
    httpd_register_uri_handler(srv, &u_dl);
    httpd_register_uri_handler(srv, &u_detail);
    httpd_register_uri_handler(srv, &u_del);
    httpd_register_uri_handler(srv, &u_rename);
    ESP_LOGI(TAG, "Dumps API registered");
}

/**
 * @file api_nfc.c
 * @brief NFC 操作相关 API：scan / read / clone-uid / write
 *
 * 长操作（read / write）走后台 task；进度通过 WebSocket 推送。
 * 全局只允许一个 NFC 任务同时运行，用 mutex 保护。
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "default_keys.h"
#include "dump_store.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_server.h"
#include "keys_store.h"
#include "mifare_classic.h"
#include "ndef.h"
#include "ntag.h"
#include "pn532.h"
#include "ws_progress.h"

static const char* TAG = "api_nfc";

static SemaphoreHandle_t s_busy = NULL;
static char              s_task_id[16] = {0};

static void make_task_id(char* out) {
    snprintf(out, 16, "t%lu", (unsigned long)(esp_random() & 0xFFFFFF));
}

static void uid_to_hex(const uint8_t* uid, uint8_t len, char* out) {
    static const char H[] = "0123456789ABCDEF";
    for (uint8_t i = 0; i < len; i++) {
        out[i * 2]     = H[uid[i] >> 4];
        out[i * 2 + 1] = H[uid[i] & 0xF];
    }
    out[len * 2] = 0;
}

static void hex_to_bytes(const char* hex, uint8_t* out, size_t* out_len) {
    size_t hl = strlen(hex);
    *out_len  = hl / 2;
    for (size_t i = 0; i < *out_len; i++) {
        char c[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        out[i]    = (uint8_t)strtoul(c, NULL, 16);
    }
}

static const char* card_type_str(pn532_card_type_t t) {
    switch (t) {
        case PN532_CARD_MIFARE_CLASSIC_1K:
            return "Mifare Classic 1K";
        case PN532_CARD_MIFARE_CLASSIC_4K:
            return "Mifare Classic 4K";
        case PN532_CARD_MIFARE_ULTRALIGHT:
            return "NTAG/Ultralight";
        default:
            return "未知";
    }
}

static const char* magic_type_str(mfc_magic_type_t t) {
    switch (t) {
        case MFC_MAGIC_GEN1A: return "Gen1a";
        case MFC_MAGIC_GEN2:  return "Gen2";
        default:              return "";
    }
}

static esp_err_t handle_scan(httpd_req_t* req) {
    pn532_target_t tgt;
    if (pn532_read_passive_target(&tgt, 800) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"未发现卡片\"}");
    }
    char uid_hex[24];
    uid_to_hex(tgt.uid, tgt.uid_len, uid_hex);

    // 检测 magic 卡类型（仅 Mifare Classic）
    mfc_magic_type_t magic = MFC_MAGIC_NONE;
    if (tgt.type == PN532_CARD_MIFARE_CLASSIC_1K || tgt.type == PN532_CARD_MIFARE_CLASSIC_4K) {
        magic = mfc_detect_magic();
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "uid", uid_hex);
    cJSON_AddNumberToObject(root, "atqa", tgt.atqa);
    cJSON_AddNumberToObject(root, "sak", tgt.sak);
    cJSON_AddNumberToObject(root, "type", tgt.type);
    cJSON_AddStringToObject(root, "typeName", card_type_str(tgt.type));
    if (magic != MFC_MAGIC_NONE) {
        cJSON_AddStringToObject(root, "magic", magic_type_str(magic));
    }
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

// 后台任务参数
typedef struct {
    char        task_id[16];
    char        name[64];  // 用户给的备注
} read_task_arg_t;

static void mfc_progress(uint8_t sector, uint8_t total, bool a, bool b, void* user) {
    const char* tid = (const char*)user;
    ws_progress_sector(tid, sector, total, a, b);
}

static void task_full_read(void* arg) {
    read_task_arg_t* a = (read_task_arg_t*)arg;

    pn532_target_t tgt;
    if (pn532_read_passive_target(&tgt, 1000) != ESP_OK) {
        ws_progress_error(a->task_id, "未发现卡片");
        goto done;
    }

    if (tgt.type == PN532_CARD_MIFARE_CLASSIC_1K || tgt.type == PN532_CARD_MIFARE_CLASSIC_4K) {
        static uint8_t keys[KEYS_USER_MAX + MIFARE_DEFAULT_KEY_COUNT][MFC_KEY_LEN];
        size_t         n = keys_store_combined(keys, sizeof(keys) / MFC_KEY_LEN);

        mfc_dump_t* dump = calloc(1, sizeof(mfc_dump_t));
        if (!dump) {
            ws_progress_error(a->task_id, "内存不足");
            goto done;
        }
        mfc_full_read(&tgt, keys, n, dump, mfc_progress, a->task_id);

        char id[40];
        dump_store_save_mfc(dump, a->name, id);

        char result[96];
        snprintf(result, sizeof(result), "{\"id\":\"%s\",\"sectors\":%d}", id, dump->sector_count);
        ws_progress_done(a->task_id, result);
        free(dump);
    } else {
        // NTAG / Ultralight
        ntag_dump_t* dump = calloc(1, sizeof(ntag_dump_t));
        if (!dump) {
            ws_progress_error(a->task_id, "内存不足");
            goto done;
        }
        ntag_full_read(&tgt, dump);
        char id[40];
        dump_store_save_ntag(dump, &tgt, a->name, id);
        char result[96];
        snprintf(result, sizeof(result), "{\"id\":\"%s\",\"pages\":%d}", id, dump->total_pages);
        ws_progress_done(a->task_id, result);
        free(dump);
    }

done:
    free(a);
    xSemaphoreGive(s_busy);
    vTaskDelete(NULL);
}

static esp_err_t handle_read(httpd_req_t* req) {
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"另一项操作正在进行\"}");
    }
    char body[160] = {0};
    int  n         = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) n = 0;
    body[n] = 0;

    read_task_arg_t* a = calloc(1, sizeof(*a));
    make_task_id(a->task_id);
    strlcpy(s_task_id, a->task_id, sizeof(s_task_id));

    cJSON* j = cJSON_Parse(body);
    if (j) {
        cJSON* nm = cJSON_GetObjectItem(j, "name");
        if (cJSON_IsString(nm)) strlcpy(a->name, nm->valuestring, sizeof(a->name));
        cJSON_Delete(j);
    }

    if (xTaskCreate(task_full_read, "nfc_read", 8192, a, 5, NULL) != pdPASS) {
        xSemaphoreGive(s_busy);
        free(a);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"任务创建失败\"}");
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"taskId\":\"%s\"}", a->task_id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t handle_clone_uid(httpd_req_t* req) {
    char body[128] = {0};
    int  n         = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"空 body\"}");
    body[n]   = 0;
    cJSON* j = cJSON_Parse(body);
    if (!j) return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"JSON 错\"}");
    cJSON* uid_j = cJSON_GetObjectItem(j, "uid");
    if (!cJSON_IsString(uid_j)) {
        cJSON_Delete(j);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"缺 uid\"}");
    }
    uint8_t uid[10];
    size_t  uid_len;
    hex_to_bytes(uid_j->valuestring, uid, &uid_len);
    cJSON_Delete(j);

    bool verify_ok = false;
    esp_err_t err = mfc_clone_uid(uid, (uint8_t)uid_len, &verify_ok);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"写入失败（可能不是魔术卡）\"}");
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"verified\":%s}", verify_ok ? "true" : "false");
    return httpd_resp_sendstr(req, resp);
}

typedef struct {
    char task_id[16];
    char dump_id[40];
} write_task_arg_t;

static void task_full_write(void* arg) {
    write_task_arg_t* a = (write_task_arg_t*)arg;
    mfc_dump_t* dump = calloc(1, sizeof(mfc_dump_t));
    if (!dump) {
        ws_progress_error(a->task_id, "内存不足");
        goto done;
    }
    if (dump_store_load_mfc(a->dump_id, dump) != ESP_OK) {
        ws_progress_error(a->task_id, "dump 加载失败");
        free(dump);
        goto done;
    }
    uint8_t verify_fails = 0;
    mfc_full_write(dump, mfc_progress, a->task_id, &verify_fails);
    char result[96];
    snprintf(result, sizeof(result), "{\"ok\":true,\"verifyFails\":%d}", verify_fails);
    ws_progress_done(a->task_id, result);
    free(dump);
done:
    free(a);
    xSemaphoreGive(s_busy);
    vTaskDelete(NULL);
}

static esp_err_t handle_write(httpd_req_t* req) {
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"另一项操作正在进行\"}");
    }
    char body[128] = {0};
    int  n         = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        xSemaphoreGive(s_busy);
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    body[n]   = 0;
    cJSON* j = cJSON_Parse(body);
    if (!j) {
        xSemaphoreGive(s_busy);
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    cJSON* did = cJSON_GetObjectItem(j, "dumpId");
    if (!cJSON_IsString(did)) {
        cJSON_Delete(j);
        xSemaphoreGive(s_busy);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"缺 dumpId\"}");
    }
    write_task_arg_t* a = calloc(1, sizeof(*a));
    strlcpy(a->dump_id, did->valuestring, sizeof(a->dump_id));
    make_task_id(a->task_id);
    cJSON_Delete(j);

    if (xTaskCreate(task_full_write, "nfc_write", 8192, a, 5, NULL) != pdPASS) {
        xSemaphoreGive(s_busy);
        free(a);
        return httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"taskId\":\"%s\"}", a->task_id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t handle_ndef_read(httpd_req_t* req) {
    pn532_target_t tgt;
    if (pn532_read_passive_target(&tgt, 800) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"未发现卡片\"}");
    }
    if (tgt.type != PN532_CARD_MIFARE_ULTRALIGHT) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"不是 NTAG/Ultralight 卡\"}");
    }

    ntag_dump_t* dump = calloc(1, sizeof(ntag_dump_t));
    if (!dump) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"内存不足\"}");
    }
    ntag_full_read(&tgt, dump);

    ndef_record_t rec;
    cJSON* root = cJSON_CreateObject();
    if (ndef_parse(dump, &rec) == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "type", rec.type == NDEF_TYPE_URI ? "uri" : "text");
        cJSON_AddStringToObject(root, "payload", rec.payload);
        if (rec.type == NDEF_TYPE_TEXT && rec.lang[0]) {
            cJSON_AddStringToObject(root, "lang", rec.lang);
        }
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "err", "未找到 NDEF 记录");
    }
    free(dump);

    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

static esp_err_t handle_ndef_write(httpd_req_t* req) {
    pn532_target_t tgt;
    if (pn532_read_passive_target(&tgt, 800) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"未发现卡片\"}");
    }
    if (tgt.type != PN532_CARD_MIFARE_ULTRALIGHT) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"不是 NTAG/Ultralight 卡\"}");
    }

    char body[512] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"空 body\"}");
    }
    body[n] = 0;

    cJSON* j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"JSON 格式错误\"}");
    }

    cJSON* type_j = cJSON_GetObjectItem(j, "type");
    esp_err_t err = ESP_FAIL;

    if (cJSON_IsString(type_j) && strcmp(type_j->valuestring, "uri") == 0) {
        cJSON* uri_j = cJSON_GetObjectItem(j, "payload");
        if (cJSON_IsString(uri_j)) {
            err = ndef_write_uri(uri_j->valuestring);
        }
    } else if (cJSON_IsString(type_j) && strcmp(type_j->valuestring, "text") == 0) {
        cJSON* text_j = cJSON_GetObjectItem(j, "payload");
        cJSON* lang_j = cJSON_GetObjectItem(j, "lang");
        if (cJSON_IsString(text_j)) {
            const char* lang = cJSON_IsString(lang_j) ? lang_j->valuestring : "en";
            err = ndef_write_text(lang, text_j->valuestring);
        }
    }
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"写入失败\"}");
}

void api_nfc_register(httpd_handle_t srv) {
    if (!s_busy) s_busy = xSemaphoreCreateBinary();
    xSemaphoreGive(s_busy);

    httpd_uri_t u_scan  = {.uri = "/api/scan", .method = HTTP_POST, .handler = handle_scan};
    httpd_uri_t u_read  = {.uri = "/api/read", .method = HTTP_POST, .handler = handle_read};
    httpd_uri_t u_clone = {.uri = "/api/clone-uid", .method = HTTP_POST, .handler = handle_clone_uid};
    httpd_uri_t u_write = {.uri = "/api/write", .method = HTTP_POST, .handler = handle_write};
    httpd_uri_t u_ndef_r = {.uri = "/api/ndef/read", .method = HTTP_POST, .handler = handle_ndef_read};
    httpd_uri_t u_ndef_w = {.uri = "/api/ndef/write", .method = HTTP_POST, .handler = handle_ndef_write};

    httpd_register_uri_handler(srv, &u_scan);
    httpd_register_uri_handler(srv, &u_read);
    httpd_register_uri_handler(srv, &u_clone);
    httpd_register_uri_handler(srv, &u_write);
    httpd_register_uri_handler(srv, &u_ndef_r);
    httpd_register_uri_handler(srv, &u_ndef_w);
    ESP_LOGI(TAG, "NFC API registered");
}

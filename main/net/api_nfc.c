/*
 * 全局只允许一个 NFC 任务同时运行；用 binary semaphore 而非 mutex 是因为
 * HTTP 任务 take、后台任务 give，不符合 mutex 所有权协议。
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "default_keys.h"
#include "dump_store.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hex_util.h"
#include "http_server.h"
#include "keys_store.h"
#include "mifare_classic.h"
#include "ndef.h"
#include "nfc_cancel.h"
#include "ntag.h"
#include "pn532.h"
#include "ws_progress.h"

static const char JSON_CT[] = "application/json";

static SemaphoreHandle_t s_busy = NULL;

static esp_err_t send_json(httpd_req_t* req, const char* s) {
    httpd_resp_set_type(req, JSON_CT);
    // cJSON_PrintUnformatted 在 OOM 时会返回 NULL,直接传给 sendstr 会 strlen(NULL) 崩掉
    return httpd_resp_sendstr(req, s ? s : "{\"ok\":false,\"err\":\"内存不足\"}");
}

static void make_task_id(char* out) {
    snprintf(out, 16, "t%lu", (unsigned long)(esp_random() & 0xFFFFFF));
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
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        return send_json(req, "{\"ok\":false,\"err\":\"另一项操作正在进行\"}");
    }
    pn532_target_t tgt;
    if (pn532_read_passive_target(&tgt, 800) != ESP_OK) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"未发现卡片\"}");
    }
    char uid_hex[24];
    hex_encode_upper(tgt.uid, tgt.uid_len, uid_hex);

    // magic 检测仅对 Mifare Classic 有意义
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
    esp_err_t err = send_json(req, s);
    free(s);
    xSemaphoreGive(s_busy);
    return err;
}

typedef struct {
    char        task_id[16];
    char        name[64];
    int64_t     created_ms;  // 设备无 RTC,时间由浏览器提供
} read_task_arg_t;

static void mfc_progress(uint8_t sector, uint8_t total, bool a, bool b, void* user) {
    const char* tid = (const char*)user;
    ws_progress_sector(tid, sector, total, a, b);
}

static void task_full_read(void* arg) {
    read_task_arg_t* a = (read_task_arg_t*)arg;
    nfc_cancel_clear();

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
        esp_err_t rerr = mfc_full_read(&tgt, keys, n, dump, mfc_progress, a->task_id);
        if (rerr == ESP_ERR_NOT_FOUND) {
            ws_progress_error(a->task_id, "卡被拿走，读取中断");
            free(dump);
            goto done;
        }
        if (rerr == ESP_ERR_TIMEOUT) {
            ws_progress_error(a->task_id, "已取消");
            free(dump);
            goto done;
        }

        char      id[40];
        esp_err_t serr = dump_store_save_mfc(dump, a->name, a->created_ms, id);
        if (serr != ESP_OK) {
            ws_progress_error(a->task_id, "dump 保存失败");
            free(dump);
            goto done;
        }

        char result[96];
        snprintf(result, sizeof(result), "{\"id\":\"%s\",\"sectors\":%d}", id, dump->sector_count);
        ws_progress_done(a->task_id, result);
        free(dump);
    } else {
        ntag_dump_t* dump = calloc(1, sizeof(ntag_dump_t));
        if (!dump) {
            ws_progress_error(a->task_id, "内存不足");
            goto done;
        }
        esp_err_t rerr = ntag_full_read(&tgt, dump);
        if (rerr == ESP_ERR_NOT_FOUND) {
            ws_progress_error(a->task_id, "卡被拿走，读取中断");
            free(dump);
            goto done;
        }
        if (rerr == ESP_ERR_TIMEOUT) {
            ws_progress_error(a->task_id, "已取消");
            free(dump);
            goto done;
        }
        char      id[40];
        esp_err_t serr = dump_store_save_ntag(dump, &tgt, a->name, a->created_ms, id);
        if (serr != ESP_OK) {
            ws_progress_error(a->task_id, "dump 保存失败");
            free(dump);
            goto done;
        }
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
        return send_json(req, "{\"ok\":false,\"err\":\"另一项操作正在进行\"}");
    }
    char body[160] = {0};
    int  n         = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) n = 0;
    body[n] = 0;

    read_task_arg_t* a = calloc(1, sizeof(*a));
    if (!a) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"内存不足\"}");
    }
    make_task_id(a->task_id);

    cJSON* j = cJSON_Parse(body);
    if (j) {
        cJSON* nm = cJSON_GetObjectItem(j, "name");
        if (cJSON_IsString(nm)) strlcpy(a->name, nm->valuestring, sizeof(a->name));
        cJSON* ts = cJSON_GetObjectItem(j, "ts");
        if (cJSON_IsNumber(ts)) a->created_ms = (int64_t)ts->valuedouble;
        cJSON_Delete(j);
    }

    // task_id 必须在 xTaskCreate 之前复制出来,之后 a 由后台任务持有
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"taskId\":\"%s\"}", a->task_id);

    if (xTaskCreate(task_full_read, "nfc_read", 8192, a, 5, NULL) != pdPASS) {
        xSemaphoreGive(s_busy);
        free(a);
        return send_json(req, "{\"ok\":false,\"err\":\"任务创建失败\"}");
    }
    return send_json(req, resp);
}

typedef struct {
    char task_id[16];
    char dump_id[40];
} write_task_arg_t;

static void task_full_write(void* arg) {
    write_task_arg_t* a = (write_task_arg_t*)arg;
    nfc_cancel_clear();
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
    esp_err_t werr = mfc_full_write(dump, mfc_progress, a->task_id, &verify_fails);
    free(dump);
    if (werr == ESP_ERR_TIMEOUT) {
        ws_progress_error(a->task_id, "已取消");
        goto done;
    }
    if (werr == ESP_ERR_NOT_FOUND) {
        ws_progress_error(a->task_id, "卡被拿走，写入中断");
        goto done;
    }
    char result[96];
    snprintf(result, sizeof(result), "{\"ok\":true,\"verifyFails\":%d}", verify_fails);
    ws_progress_done(a->task_id, result);
done:
    free(a);
    xSemaphoreGive(s_busy);
    vTaskDelete(NULL);
}

static esp_err_t handle_write(httpd_req_t* req) {
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        return send_json(req, "{\"ok\":false,\"err\":\"另一项操作正在进行\"}");
    }
    char body[128] = {0};
    int  n         = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false}");
    }
    body[n]  = 0;
    cJSON* j = cJSON_Parse(body);
    if (!j) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false}");
    }
    cJSON* did = cJSON_GetObjectItem(j, "dumpId");
    if (!cJSON_IsString(did) || !id_is_safe(did->valuestring, 39)) {
        cJSON_Delete(j);
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"缺 dumpId 或非法\"}");
    }
    write_task_arg_t* a = calloc(1, sizeof(*a));
    if (!a) {
        cJSON_Delete(j);
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"内存不足\"}");
    }
    strlcpy(a->dump_id, did->valuestring, sizeof(a->dump_id));
    make_task_id(a->task_id);
    cJSON_Delete(j);

    // task_id 必须在 xTaskCreate 之前复制出来,之后 a 由后台任务持有
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"taskId\":\"%s\"}", a->task_id);

    if (xTaskCreate(task_full_write, "nfc_write", 8192, a, 5, NULL) != pdPASS) {
        xSemaphoreGive(s_busy);
        free(a);
        return send_json(req, "{\"ok\":false}");
    }
    return send_json(req, resp);
}

static esp_err_t handle_cancel(httpd_req_t* req) {
    nfc_cancel_request();
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t handle_ndef_read(httpd_req_t* req) {
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        return send_json(req, "{\"ok\":false,\"err\":\"另一项操作正在进行\"}");
    }
    pn532_target_t tgt;
    if (pn532_read_passive_target(&tgt, 800) != ESP_OK) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"未发现卡片\"}");
    }
    if (tgt.type != PN532_CARD_MIFARE_ULTRALIGHT) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"不是 NTAG/Ultralight 卡\"}");
    }

    ntag_dump_t* dump = calloc(1, sizeof(ntag_dump_t));
    if (!dump) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"内存不足\"}");
    }
    esp_err_t rerr = ntag_full_read(&tgt, dump);
    if (rerr != ESP_OK) {
        free(dump);
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"读取失败\"}");
    }

    ndef_record_t rec;
    cJSON*        root = cJSON_CreateObject();
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
    esp_err_t err = send_json(req, s);
    free(s);
    xSemaphoreGive(s_busy);
    return err;
}

static esp_err_t handle_ndef_write(httpd_req_t* req) {
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        return send_json(req, "{\"ok\":false,\"err\":\"另一项操作正在进行\"}");
    }
    pn532_target_t tgt;
    if (pn532_read_passive_target(&tgt, 800) != ESP_OK) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"未发现卡片\"}");
    }
    if (tgt.type != PN532_CARD_MIFARE_ULTRALIGHT) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"不是 NTAG/Ultralight 卡\"}");
    }

    char body[512] = {0};
    int  n         = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"空 body\"}");
    }
    body[n] = 0;

    cJSON* j = cJSON_Parse(body);
    if (!j) {
        xSemaphoreGive(s_busy);
        return send_json(req, "{\"ok\":false,\"err\":\"JSON 格式错误\"}");
    }

    cJSON*    type_j = cJSON_GetObjectItem(j, "type");
    esp_err_t err    = ESP_FAIL;

    if (cJSON_IsString(type_j) && strcmp(type_j->valuestring, "uri") == 0) {
        cJSON* uri_j = cJSON_GetObjectItem(j, "payload");
        if (cJSON_IsString(uri_j)) err = ndef_write_uri(uri_j->valuestring);
    } else if (cJSON_IsString(type_j) && strcmp(type_j->valuestring, "text") == 0) {
        cJSON* text_j = cJSON_GetObjectItem(j, "payload");
        cJSON* lang_j = cJSON_GetObjectItem(j, "lang");
        if (cJSON_IsString(text_j)) {
            const char* lang = cJSON_IsString(lang_j) ? lang_j->valuestring : "en";
            err              = ndef_write_text(lang, text_j->valuestring);
        }
    }
    cJSON_Delete(j);
    xSemaphoreGive(s_busy);

    return send_json(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"写入失败\"}");
}

void api_nfc_register(httpd_handle_t srv) {
    if (!s_busy) {
        s_busy = xSemaphoreCreateBinary();
        xSemaphoreGive(s_busy);
    }

    httpd_uri_t u_scan   = {.uri = "/api/scan", .method = HTTP_POST, .handler = handle_scan};
    httpd_uri_t u_read   = {.uri = "/api/read", .method = HTTP_POST, .handler = handle_read};
    httpd_uri_t u_write  = {.uri = "/api/write", .method = HTTP_POST, .handler = handle_write};
    httpd_uri_t u_ndef_r = {.uri = "/api/ndef/read", .method = HTTP_POST, .handler = handle_ndef_read};
    httpd_uri_t u_ndef_w = {.uri = "/api/ndef/write", .method = HTTP_POST, .handler = handle_ndef_write};
    httpd_uri_t u_cancel = {.uri = "/api/cancel", .method = HTTP_POST, .handler = handle_cancel};

    httpd_register_uri_handler(srv, &u_scan);
    httpd_register_uri_handler(srv, &u_read);
    httpd_register_uri_handler(srv, &u_write);
    httpd_register_uri_handler(srv, &u_ndef_r);
    httpd_register_uri_handler(srv, &u_ndef_w);
    httpd_register_uri_handler(srv, &u_cancel);
}

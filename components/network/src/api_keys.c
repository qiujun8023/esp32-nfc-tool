/**
 * @file api_keys.c
 * @brief 密钥管理 API
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "default_keys.h"
#include "esp_log.h"
#include "hex_util.h"
#include "http_server.h"
#include "keys_store.h"
#include "mifare_classic.h"

static const char* TAG = "api_keys";

static const char JSON_CT[] = "application/json";

static esp_err_t send_json(httpd_req_t* req, const char* s) {
    httpd_resp_set_type(req, JSON_CT);
    return httpd_resp_sendstr(req, s);
}

static esp_err_t handle_get(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();

    cJSON* defaults = cJSON_CreateArray();
    for (size_t i = 0; i < MIFARE_DEFAULT_KEY_COUNT; i++) {
        char hex[13];
        hex_encode_upper(MIFARE_DEFAULT_KEYS[i], MFC_KEY_LEN, hex);
        cJSON_AddItemToArray(defaults, cJSON_CreateString(hex));
    }
    cJSON_AddItemToObject(root, "defaults", defaults);

    cJSON*  user = cJSON_CreateArray();
    uint8_t buf[KEYS_USER_MAX][MFC_KEY_LEN];
    size_t  n = keys_store_load_user(buf, KEYS_USER_MAX);
    for (size_t i = 0; i < n; i++) {
        char hex[13];
        hex_encode_upper(buf[i], MFC_KEY_LEN, hex);
        cJSON_AddItemToArray(user, cJSON_CreateString(hex));
    }
    cJSON_AddItemToObject(root, "user", user);

    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t err = send_json(req, s);
    free(s);
    return err;
}

static esp_err_t handle_post(httpd_req_t* req) {
    char body[96] = {0};
    int  n        = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return send_json(req, "{\"ok\":false,\"err\":\"空 body\"}");
    body[n] = 0;

    cJSON* j = cJSON_Parse(body);
    if (!j) return send_json(req, "{\"ok\":false,\"err\":\"JSON\"}");

    cJSON* k = cJSON_GetObjectItem(j, "key");
    if (!cJSON_IsString(k) || !hex_is_valid(k->valuestring, 12)) {
        cJSON_Delete(j);
        return send_json(req, "{\"ok\":false,\"err\":\"key 必须是 12 字符 hex\"}");
    }
    uint8_t key[MFC_KEY_LEN];
    size_t  klen = 0;
    hex_decode(k->valuestring, key, sizeof(key), &klen, true);
    cJSON_Delete(j);
    if (klen != MFC_KEY_LEN) return send_json(req, "{\"ok\":false,\"err\":\"key 长度错\"}");

    esp_err_t err = keys_store_add(key);
    return send_json(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"满\"}");
}

static esp_err_t handle_delete(httpd_req_t* req) {
    // /api/keys/<index>
    const char* p = strrchr(req->uri, '/');
    if (!p) return send_json(req, "{\"ok\":false,\"err\":\"缺 index\"}");
    p++;
    // 必须为纯数字且长度合理
    if (!*p) return send_json(req, "{\"ok\":false,\"err\":\"index 非法\"}");
    for (const char* c = p; *c; c++) {
        if (*c < '0' || *c > '9') return send_json(req, "{\"ok\":false,\"err\":\"index 非法\"}");
    }
    long idx = strtol(p, NULL, 10);
    if (idx < 0 || idx >= KEYS_USER_MAX) return send_json(req, "{\"ok\":false,\"err\":\"index 越界\"}");

    esp_err_t err = keys_store_remove((size_t)idx);
    return send_json(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
}

void api_keys_register(httpd_handle_t srv) {
    httpd_uri_t u_get  = {.uri = "/api/keys", .method = HTTP_GET, .handler = handle_get};
    httpd_uri_t u_post = {.uri = "/api/keys", .method = HTTP_POST, .handler = handle_post};
    httpd_uri_t u_del  = {.uri = "/api/keys/*", .method = HTTP_DELETE, .handler = handle_delete};
    httpd_register_uri_handler(srv, &u_get);
    httpd_register_uri_handler(srv, &u_post);
    httpd_register_uri_handler(srv, &u_del);
    ESP_LOGI(TAG, "Keys API registered");
}

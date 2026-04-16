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
#include "http_server.h"
#include "keys_store.h"
#include "mifare_classic.h"

static const char* TAG = "api_keys";

static void key_to_hex(const uint8_t* k, char out[13]) {
    static const char H[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        out[i * 2]     = H[k[i] >> 4];
        out[i * 2 + 1] = H[k[i] & 0xF];
    }
    out[12] = 0;
}

static esp_err_t handle_get(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();

    cJSON* defaults = cJSON_CreateArray();
    for (size_t i = 0; i < MIFARE_DEFAULT_KEY_COUNT; i++) {
        char hex[13];
        key_to_hex(MIFARE_DEFAULT_KEYS[i], hex);
        cJSON_AddItemToArray(defaults, cJSON_CreateString(hex));
    }
    cJSON_AddItemToObject(root, "defaults", defaults);

    cJSON*  user = cJSON_CreateArray();
    uint8_t buf[KEYS_USER_MAX][MFC_KEY_LEN];
    size_t  n = keys_store_load_user(buf, KEYS_USER_MAX);
    for (size_t i = 0; i < n; i++) {
        char hex[13];
        key_to_hex(buf[i], hex);
        cJSON_AddItemToArray(user, cJSON_CreateString(hex));
    }
    cJSON_AddItemToObject(root, "user", user);

    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

static esp_err_t handle_post(httpd_req_t* req) {
    char body[64] = {0};
    int  n        = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return httpd_resp_sendstr(req, "{\"ok\":false}");
    body[n]   = 0;
    cJSON* j = cJSON_Parse(body);
    if (!j) return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"JSON\"}");
    cJSON* k = cJSON_GetObjectItem(j, "key");
    if (!cJSON_IsString(k) || strlen(k->valuestring) != 12) {
        cJSON_Delete(j);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"key 必须是 12 字符 hex\"}");
    }
    uint8_t key[6];
    for (int i = 0; i < 6; i++) {
        char c[3] = {k->valuestring[i * 2], k->valuestring[i * 2 + 1], 0};
        key[i]    = (uint8_t)strtoul(c, NULL, 16);
    }
    cJSON_Delete(j);

    esp_err_t err = keys_store_add(key);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"满\"}");
}

static esp_err_t handle_delete(httpd_req_t* req) {
    // /api/keys/<index>
    const char* p = strrchr(req->uri, '/');
    if (!p) return httpd_resp_sendstr(req, "{\"ok\":false}");
    int idx = atoi(p + 1);
    keys_store_remove((size_t)idx);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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

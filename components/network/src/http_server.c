/**
 * @file http_server.c
 * @brief HTTP 服务器：静态资源 + REST API + WebSocket
 */

#include "http_server.h"

#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_chip_info.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "pn532.h"
#include "ws_progress.h"

static const char* TAG = "http";

/* -------- captive portal 客户端状态跟踪 -------- */

static uint32_t s_portal_done[WIFI_AP_MAX_CONN];
static uint8_t  s_portal_count = 0;

static uint32_t get_client_ip(httpd_req_t* req) {
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr*)&addr, &len) != 0) return 0;
    return addr.sin_addr.s_addr;
}

static bool portal_is_done(uint32_t ip) {
    for (int i = 0; i < s_portal_count; i++) {
        if (s_portal_done[i] == ip) return true;
    }
    return false;
}

static void portal_mark_done(uint32_t ip) {
    if (!ip || portal_is_done(ip)) return;
    if (s_portal_count < WIFI_AP_MAX_CONN) {
        s_portal_done[s_portal_count++] = ip;
    }
}

void portal_clear_all(void) {
    s_portal_count = 0;
}

#include "web_ui.h"

esp_err_t handle_index(httpd_req_t* req) {
    portal_mark_done(get_client_ip(req));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

esp_err_t handle_app_js(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req, app_js_start, app_js_end - app_js_start - 1);
}

esp_err_t handle_style_css(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, style_css_start, style_css_end - style_css_start - 1);
}

/**
 * @brief 对已通过 portal 的客户端，返回各平台预期的"有网络"响应
 *
 * 这样设备不再判定为 captive portal / 无网络，保持 WiFi 稳定连接。
 */
static esp_err_t reply_connectivity_success(httpd_req_t* req) {
    const char* uri = req->uri;

    /* Android / Chrome */
    if (strstr(uri, "generate_204") || strstr(uri, "gen_204")) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    /* Apple iOS / macOS */
    if (strstr(uri, "hotspot-detect.html")) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_sendstr(req,
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
            "<BODY>Success</BODY></HTML>");
    }
    /* Windows */
    if (strstr(uri, "connecttest.txt")) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Microsoft Connect Test");
    }
    if (strstr(uri, "ncsi.txt")) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Microsoft NCSI");
    }
    /* Firefox */
    if (strstr(uri, "canonical.html") || strstr(uri, "success.txt")) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "success\n");
    }

    /* 非探测 URL（用户在浏览器随便输了什么）→ 重定向到主页 */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/**
 * @brief captive portal 通配处理
 *
 * 流程：
 *  1. 新设备首次连接 → 302 重定向 → 触发系统 captive portal 弹窗，展示工具 UI
 *  2. 用户加载主页后标记为"已通过"（见 handle_index）
 *  3. 之后的连通性探测 → 返回各平台预期的成功响应 → 设备保持连接
 */
esp_err_t handle_redirect(httpd_req_t* req) {
    uint32_t ip = get_client_ip(req);
    if (ip && portal_is_done(ip)) {
        return reply_connectivity_success(req);
    }
    /* 新客户端：302 重定向，触发 captive portal 弹窗 */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t handle_status(httpd_req_t* req) {
    size_t total = 0, used = 0;
    esp_littlefs_info(LFS_PARTITION_LABEL, &total, &used);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", APP_VERSION);
    cJSON_AddNumberToObject(root, "pn532Fw", pn532_get_firmware_version());
    cJSON_AddNumberToObject(root, "fsTotal", total);
    cJSON_AddNumberToObject(root, "fsUsed", used);
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "freeHeap", esp_get_free_heap_size());

    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}

httpd_handle_t http_server_start(void) {
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.max_uri_handlers  = 32;
    cfg.stack_size        = 8192;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return NULL;
    }

    httpd_uri_t u_index   = {.uri = "/", .method = HTTP_GET, .handler = handle_index};
    httpd_uri_t u_js      = {.uri = "/app.js", .method = HTTP_GET, .handler = handle_app_js};
    httpd_uri_t u_css     = {.uri = "/style.css", .method = HTTP_GET, .handler = handle_style_css};
    httpd_uri_t u_status  = {.uri = "/api/status", .method = HTTP_GET, .handler = handle_status};
    httpd_uri_t u_ws      = {.uri = "/ws", .method = HTTP_GET, .handler = ws_progress_handler, .is_websocket = true};

    httpd_register_uri_handler(srv, &u_index);
    httpd_register_uri_handler(srv, &u_js);
    httpd_register_uri_handler(srv, &u_css);
    httpd_register_uri_handler(srv, &u_status);
    httpd_register_uri_handler(srv, &u_ws);

    api_nfc_register(srv);
    api_dumps_register(srv);
    api_keys_register(srv);
    api_ota_register(srv);

    // captive portal 探测 URL
    httpd_uri_t u_catch = {.uri = "/*", .method = HTTP_GET, .handler = handle_redirect};
    httpd_register_uri_handler(srv, &u_catch);

    ws_progress_init(srv);
    ESP_LOGI(TAG, "http server started");
    return srv;
}

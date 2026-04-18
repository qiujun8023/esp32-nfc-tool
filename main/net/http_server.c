#include "http_server.h"

#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_chip_info.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "pn532.h"
#include "ws_progress.h"

static const char* TAG = "http";

/*
 * captive portal 客户端状态跟踪：用环形数组记录已加载过主页的客户端 IP，
 * 之后这些 IP 的系统连通性探测会被回复为成功，浏览器不再弹 captive 页。
 */
static uint32_t          s_portal_done[CONFIG_ESP_MAX_STA_CONN];
static uint8_t           s_portal_count = 0;
static uint8_t           s_portal_head  = 0;
static SemaphoreHandle_t s_portal_lock  = NULL;

static uint32_t get_client_ip(httpd_req_t* req) {
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr*)&addr, &len) != 0) return 0;
    return addr.sin_addr.s_addr;
}

static bool portal_is_done_locked(uint32_t ip) {
    for (int i = 0; i < s_portal_count; i++) {
        if (s_portal_done[i] == ip) return true;
    }
    return false;
}

static bool portal_is_done(uint32_t ip) {
    if (!s_portal_lock) return false;
    xSemaphoreTake(s_portal_lock, portMAX_DELAY);
    bool r = portal_is_done_locked(ip);
    xSemaphoreGive(s_portal_lock);
    return r;
}

// 环形覆盖: 满后最老条目被新 IP 替换,避免长时间运行后新客户端一直收到 captive 页
static void portal_mark_done(uint32_t ip) {
    if (!ip || !s_portal_lock) return;
    xSemaphoreTake(s_portal_lock, portMAX_DELAY);
    if (!portal_is_done_locked(ip)) {
        s_portal_done[s_portal_head] = ip;
        s_portal_head                = (s_portal_head + 1) % CONFIG_ESP_MAX_STA_CONN;
        if (s_portal_count < CONFIG_ESP_MAX_STA_CONN) s_portal_count++;
    }
    xSemaphoreGive(s_portal_lock);
}

void http_server_portal_clear_all(void) {
    if (!s_portal_lock) return;
    xSemaphoreTake(s_portal_lock, portMAX_DELAY);
    s_portal_count = 0;
    s_portal_head  = 0;
    xSemaphoreGive(s_portal_lock);
}

// 前端静态资源由 EMBED_FILES 生成链接符号
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");
extern const char style_css_start[]  asm("_binary_style_css_start");
extern const char style_css_end[]    asm("_binary_style_css_end");

static esp_err_t handle_index(httpd_req_t* req) {
    portal_mark_done(get_client_ip(req));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t handle_app_js(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req, app_js_start, app_js_end - app_js_start);
}

static esp_err_t handle_style_css(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, style_css_start, style_css_end - style_css_start);
}

/*
 * 对已加载过主页的客户端，返回各平台期望的「有网络」响应，
 * 否则系统会认定为 captive portal 未通过 / 无网络，自动断开 Wi-Fi。
 */
static esp_err_t reply_connectivity_success(httpd_req_t* req) {
    const char* uri = req->uri;

    // Android / Chrome
    if (strstr(uri, "generate_204") || strstr(uri, "gen_204")) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    // Apple iOS / macOS
    if (strstr(uri, "hotspot-detect.html")) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_sendstr(req,
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
            "<BODY>Success</BODY></HTML>");
    }
    // Windows
    if (strstr(uri, "connecttest.txt")) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Microsoft Connect Test");
    }
    if (strstr(uri, "ncsi.txt")) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Microsoft NCSI");
    }
    // Firefox
    if (strstr(uri, "canonical.html") || strstr(uri, "success.txt")) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "success\n");
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    // iOS 需要非空 body 才会判定为 captive portal 并触发弹窗
    return httpd_resp_sendstr(req, "Redirect to captive portal");
}

/*
 * captive portal 兜底处理:
 *  - 新客户端未加载主页: 303 重定向到主页,触发系统 portal 弹窗
 *  - 已加载过主页: 按各平台探测协议回"有网络",避免系统把 AP 判为无网后断开
 */
static esp_err_t handle_redirect(httpd_req_t* req) {
    uint32_t ip = get_client_ip(req);
    if (ip && portal_is_done(ip)) {
        return reply_connectivity_success(req);
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    // iOS 需要非空 body 才会判定为 captive portal 并触发弹窗
    return httpd_resp_sendstr(req, "Redirect to captive portal");
}

static esp_err_t handle_status(httpd_req_t* req) {
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

// 所有未注册 URL 在此兜底: 重定向或回连通性探测
static esp_err_t handle_404_redirect(httpd_req_t* req, httpd_err_code_t err) {
    (void)err;
    return handle_redirect(req);
}

httpd_handle_t http_server_start(void) {
    if (!s_portal_lock) s_portal_lock = xSemaphoreCreateMutex();

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.max_uri_handlers  = 32;
    cfg.stack_size        = 8192;
    cfg.max_open_sockets  = 13;

    // captive 兜底会产生大量 404/断连,降级相关子模块日志避免刷屏
    esp_log_level_set("httpd_uri",   ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx",  ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

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

    // 用 404 err handler 而非通配 URI,避免通配符吞掉上面已注册的具体 handler
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, handle_404_redirect);

    ws_progress_init(srv);
    ESP_LOGI(TAG, "http server started");
    return srv;
}

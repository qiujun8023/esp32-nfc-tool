#pragma once

#include "esp_http_server.h"

httpd_handle_t http_server_start(void);

// 给 api_*.c 用的注册函数
void api_nfc_register(httpd_handle_t srv);
void api_dumps_register(httpd_handle_t srv);
void api_keys_register(httpd_handle_t srv);
void api_ota_register(httpd_handle_t srv);

// 静态资源 handler（在 http_server.c 内）
esp_err_t handle_index(httpd_req_t* req);
esp_err_t handle_app_js(httpd_req_t* req);
esp_err_t handle_style_css(httpd_req_t* req);
esp_err_t handle_redirect(httpd_req_t* req);
esp_err_t handle_status(httpd_req_t* req);

// STA 断开时调用，清除该客户端的 captive portal 状态
void portal_clear_all(void);

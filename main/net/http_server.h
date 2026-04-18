#pragma once

#include "esp_http_server.h"

httpd_handle_t http_server_start(void);

void api_nfc_register(httpd_handle_t srv);
void api_dumps_register(httpd_handle_t srv);
void api_keys_register(httpd_handle_t srv);
void api_ota_register(httpd_handle_t srv);

// STA 断开时调用,清除所有记录的 captive portal 状态
void http_server_portal_clear_all(void);

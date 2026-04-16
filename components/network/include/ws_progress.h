#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_http_server.h"

void ws_progress_init(httpd_handle_t srv);

// 注册 /ws 路由（在 http_server 初始化时调用）
esp_err_t ws_progress_handler(httpd_req_t* req);

// 广播一条 JSON 文本到所有 WS 客户端
void ws_progress_broadcast(const char* json);

// 便捷：扇区进度
void ws_progress_sector(const char* task_id, uint8_t sector, uint8_t total,
                        bool key_a_found, bool key_b_found);

// 便捷：完成 / 错误
void ws_progress_done(const char* task_id, const char* result_json);
void ws_progress_error(const char* task_id, const char* msg);

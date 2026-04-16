/**
 * @file ws_progress.c
 * @brief WebSocket /ws 端点 + 进度广播
 */

#include "ws_progress.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "ws";
static httpd_handle_t s_srv = NULL;

#define MAX_CLIENTS 4
#define MAX_FDS 16

void ws_progress_init(httpd_handle_t srv) {
    s_srv = srv;
}

esp_err_t ws_progress_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "ws client connected");
        return ESP_OK;
    }
    // 收到帧（我们不需要处理客户端消息，丢弃即可）
    httpd_ws_frame_t f = {0};
    esp_err_t        err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.len > 0 && f.len < 256) {
        uint8_t buf[256];
        f.payload = buf;
        httpd_ws_recv_frame(req, &f, f.len);
    }
    return ESP_OK;
}

void ws_progress_broadcast(const char* json) {
    if (!s_srv || !json) return;
    size_t fds_count = MAX_FDS;
    int    fds[MAX_FDS];
    if (httpd_get_client_list(s_srv, &fds_count, fds) != ESP_OK) return;

    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t*)json,
        .len     = strlen(json),
    };
    for (size_t i = 0; i < fds_count; i++) {
        if (httpd_ws_get_fd_info(s_srv, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
        if (httpd_ws_send_frame_async(s_srv, fds[i], &pkt) != ESP_OK) {
            httpd_sess_trigger_close(s_srv, fds[i]);
        }
    }
}

void ws_progress_sector(const char* task_id, uint8_t sector, uint8_t total,
                        bool key_a_found, bool key_b_found) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"task\":\"%s\",\"type\":\"progress\",\"sector\":%d,\"total\":%d,\"keyA\":%s,\"keyB\":%s}",
             task_id ? task_id : "", sector, total, key_a_found ? "true" : "false",
             key_b_found ? "true" : "false");
    ws_progress_broadcast(buf);
}

void ws_progress_done(const char* task_id, const char* result_json) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"task\":\"%s\",\"type\":\"done\",\"result\":%s}",
             task_id ? task_id : "", result_json ? result_json : "{}");
    ws_progress_broadcast(buf);
}

void ws_progress_error(const char* task_id, const char* msg) {
    char buf[200];
    snprintf(buf, sizeof(buf), "{\"task\":\"%s\",\"type\":\"error\",\"msg\":\"%s\"}",
             task_id ? task_id : "", msg ? msg : "");
    ws_progress_broadcast(buf);
}

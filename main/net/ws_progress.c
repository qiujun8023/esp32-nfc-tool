#include "ws_progress.h"

#include <stdio.h>
#include <string.h>

static httpd_handle_t s_srv = NULL;

#define WS_MAX_FDS         16
#define WS_MAX_FRAME_SIZE  256

void ws_progress_init(httpd_handle_t srv) {
    s_srv = srv;
}

esp_err_t ws_progress_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        return ESP_OK;
    }
    // 单向广播，客户端发来的帧一律丢弃（但必须 recv 掉以免占用 httpd buffer）
    httpd_ws_frame_t f   = {0};
    esp_err_t        err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.len > 0 && f.len < WS_MAX_FRAME_SIZE) {
        uint8_t buf[WS_MAX_FRAME_SIZE];
        f.payload = buf;
        httpd_ws_recv_frame(req, &f, f.len);
    }
    return ESP_OK;
}

void ws_progress_broadcast(const char* json) {
    if (!s_srv || !json) return;
    size_t fds_count = WS_MAX_FDS;
    int    fds[WS_MAX_FDS];
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
             task_id ? task_id : "", sector, total,
             key_a_found ? "true" : "false", key_b_found ? "true" : "false");
    ws_progress_broadcast(buf);
}

void ws_progress_done(const char* task_id, const char* result_json) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"task\":\"%s\",\"type\":\"done\",\"result\":%s}",
             task_id ? task_id : "", result_json ? result_json : "{}");
    ws_progress_broadcast(buf);
}

// 错误消息直接拼进 JSON,必须转义 " \ 及控制字符,否则客户端会收到坏 JSON
static void json_escape(const char* src, char* dst, size_t dst_size) {
    if (dst_size == 0) return;
    size_t j    = 0;
    size_t cap  = dst_size - 1;
    for (size_t i = 0; src[i] && j < cap; i++) {
        unsigned char c   = (unsigned char)src[i];
        const char*   esc = NULL;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            case '\b': esc = "\\b";  break;
            case '\f': esc = "\\f";  break;
            default:
                if (c < 0x20) {
                    if (j + 6 > cap) break;
                    static const char H[] = "0123456789abcdef";
                    dst[j++] = '\\';
                    dst[j++] = 'u';
                    dst[j++] = '0';
                    dst[j++] = '0';
                    dst[j++] = H[(c >> 4) & 0xF];
                    dst[j++] = H[c & 0xF];
                    continue;
                }
                dst[j++] = (char)c;
                continue;
        }
        size_t elen = strlen(esc);
        if (j + elen > cap) break;
        memcpy(dst + j, esc, elen);
        j += elen;
    }
    dst[j] = 0;
}

// 三个字符串字段都来自受控来源（hex_encode_upper / 静态常量），无 " \ 控制字符，不需 json_escape
void ws_card_in(const char* uid_hex, uint8_t card_type, const char* type_name, const char* magic) {
    char magic_field[32] = "";
    if (magic && magic[0]) {
        snprintf(magic_field, sizeof(magic_field), ",\"magic\":\"%s\"", magic);
    }
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"card_in\",\"uid\":\"%s\",\"cardType\":%d,\"typeName\":\"%s\"%s}",
             uid_hex ? uid_hex : "", card_type,
             type_name ? type_name : "", magic_field);
    ws_progress_broadcast(buf);
}

void ws_card_meta(const char* magic) {
    if (!magic || !magic[0]) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"card_meta\",\"magic\":\"%s\"}", magic);
    ws_progress_broadcast(buf);
}

void ws_card_out(void) {
    ws_progress_broadcast("{\"type\":\"card_out\"}");
}

void ws_progress_error(const char* task_id, const char* msg) {
    char escaped_msg[160];
    json_escape(msg ? msg : "", escaped_msg, sizeof(escaped_msg));
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"task\":\"%s\",\"type\":\"error\",\"msg\":\"%s\"}",
             task_id ? task_id : "", escaped_msg);
    ws_progress_broadcast(buf);
}

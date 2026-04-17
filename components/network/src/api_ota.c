/**
 * @file api_ota.c
 * @brief Web OTA 固件更新 API
 *
 * POST /api/ota  接收 .bin 固件文件，分块写入 OTA 分区，完成后重启
 */

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"

static const char* TAG = "api_ota";

static esp_err_t handle_ota(httpd_req_t* req) {
    size_t total = req->content_len;
    if (total == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"空文件\"}");
    }

    const esp_partition_t* update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"找不到 OTA 分区\"}");
    }

    ESP_LOGI(TAG, "ota start: partition=%s size=%u", update->label, (unsigned)total);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update, total, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"OTA begin 失败\"}");
    }

    char buf[1024];
    size_t received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf, sizeof(buf));
        if (n <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"接收中断\"}");
        }
        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %u: %s", (unsigned)received, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"写入失败\"}");
        }
        received += n;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"OTA 校验失败\"}");
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"设置启动分区失败\"}");
    }

    ESP_LOGI(TAG, "ota done, rebooting");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"固件更新完成，即将重启\"}");

    // 延迟 1 秒后重启，让 HTTP 响应先发出去
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;  // 实际不会到达
}

void api_ota_register(httpd_handle_t srv) {
    httpd_uri_t u_ota = {.uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota};
    httpd_register_uri_handler(srv, &u_ota);
    ESP_LOGI(TAG, "ota api registered");
}

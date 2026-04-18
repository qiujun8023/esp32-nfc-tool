#include <string.h>

#include "esp_app_format.h"
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

    ESP_LOGI(TAG, "ota start, partition=%s size=%u", update->label, (unsigned)total);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update, total, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"OTA begin 失败\"}");
    }

    char buf[1024];
    size_t received = 0;
    bool header_checked = false;
    while (received < total) {
        int n = httpd_req_recv(req, buf, sizeof(buf));
        if (n <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"接收中断\"}");
        }
        // ESP 镜像首字节 magic = 0xE9,首包不合法则直接中止,避免把非固件写入分区
        if (!header_checked) {
            if (n < (int)sizeof(esp_image_header_t) ||
                (uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
                ESP_LOGE(TAG, "invalid image header, first byte=0x%02x", (uint8_t)buf[0]);
                esp_ota_abort(ota_handle);
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"非法固件格式\"}");
            }
            header_checked = true;
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

    // 让 HTTP 响应发完再复位
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

void api_ota_register(httpd_handle_t srv) {
    httpd_uri_t u_ota = {.uri = "/api/ota", .method = HTTP_POST, .handler = handle_ota};
    httpd_register_uri_handler(srv, &u_ota);
}

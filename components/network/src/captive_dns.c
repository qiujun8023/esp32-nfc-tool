/**
 * @file captive_dns.c
 * @brief 简易 DNS 劫持：所有 A 查询都返回 192.168.4.1
 */

#include "captive_dns.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "captive_dns";

static void dns_task(void* arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket fail");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind 53 fail");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t            buf[512];
    struct sockaddr_in cli;

    while (1) {
        socklen_t clen = sizeof(cli);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&cli, &clen);
        if (n < 12) continue;
        if (n > 512 - 16) continue;  // 确保追加应答记录后不溢出
        uint8_t resp[512];
        memcpy(resp, buf, n);
        resp[2] = 0x81;
        resp[3] = 0x80;
        resp[6] = 0x00;
        resp[7] = 0x01;
        int p   = n;
        // 答案：name pointer (0xC00C), type=A, class=IN, ttl=60, rdlen=4, ip=192.168.4.1
        resp[p++] = 0xC0;
        resp[p++] = 0x0C;
        resp[p++] = 0x00;
        resp[p++] = 0x01;
        resp[p++] = 0x00;
        resp[p++] = 0x01;
        resp[p++] = 0x00;
        resp[p++] = 0x00;
        resp[p++] = 0x00;
        resp[p++] = 60;
        resp[p++] = 0x00;
        resp[p++] = 0x04;
        resp[p++] = 192;
        resp[p++] = 168;
        resp[p++] = 4;
        resp[p++] = 1;
        sendto(sock, resp, p, 0, (struct sockaddr*)&cli, clen);
    }
}

void captive_dns_start(void) {
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "captive DNS started");
}

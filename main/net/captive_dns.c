#include "captive_dns.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "captive_dns";

/* DNS 报文头字段偏移(RFC1035 4.1.1):flags=2-3,ancount=6-7 */
#define DNS_HDR_LEN      12
#define DNS_FLAGS_HI     2
#define DNS_FLAGS_LO     3
#define DNS_ANCOUNT_HI   6
#define DNS_ANCOUNT_LO   7

/* 响应标志位: QR=1 response,AA=1 权威,RD=1 desired,RA=1 recursion available */
#define DNS_FLAGS_HI_RESP 0x81
#define DNS_FLAGS_LO_RESP 0x80
#define DNS_ANSWER_COUNT  0x01

/* 应答记录固定 16 字节:2ptr + 2type + 2class + 4ttl + 2rdlen + 4ip */
#define DNS_ANSWER_LEN 16

/* 所有 A 查询都劫持到 AP IP:192.168.4.1 */
static const uint8_t AP_IP[4] = {192, 168, 4, 1};
static const uint32_t A_TTL_SEC = 60;

static void dns_task(void* arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind udp 53 failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t            buf[512];
    struct sockaddr_in cli;

    while (1) {
        socklen_t clen = sizeof(cli);
        int       n    = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&cli, &clen);
        /* 过滤过短查询 / 追加应答后会溢出 buf 的长帧 */
        if (n < DNS_HDR_LEN || n > (int)sizeof(buf) - DNS_ANSWER_LEN)
            continue;

        /* 原样复制查询,改写 header 标志 + ancount,再追加 A 记录 */
        uint8_t resp[512];
        memcpy(resp, buf, n);
        resp[DNS_FLAGS_HI]   = DNS_FLAGS_HI_RESP;
        resp[DNS_FLAGS_LO]   = DNS_FLAGS_LO_RESP;
        resp[DNS_ANCOUNT_HI] = 0x00;
        resp[DNS_ANCOUNT_LO] = DNS_ANSWER_COUNT;

        int p = n;
        /* NAME: 指针压缩,指向 header 后首个问题名 (0xC00C) */
        resp[p++] = 0xC0;
        resp[p++] = 0x0C;
        /* TYPE=A(0x0001) */
        resp[p++] = 0x00;
        resp[p++] = 0x01;
        /* CLASS=IN(0x0001) */
        resp[p++] = 0x00;
        resp[p++] = 0x01;
        /* TTL(4 字节,网络字节序)*/
        resp[p++] = (A_TTL_SEC >> 24) & 0xFF;
        resp[p++] = (A_TTL_SEC >> 16) & 0xFF;
        resp[p++] = (A_TTL_SEC >> 8) & 0xFF;
        resp[p++] = A_TTL_SEC & 0xFF;
        /* RDLENGTH=4 */
        resp[p++] = 0x00;
        resp[p++] = 0x04;
        /* RDATA: 192.168.4.1 */
        resp[p++] = AP_IP[0];
        resp[p++] = AP_IP[1];
        resp[p++] = AP_IP[2];
        resp[p++] = AP_IP[3];

        sendto(sock, resp, p, 0, (struct sockaddr*)&cli, clen);
    }
}

void captive_dns_start(void) {
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, NULL);
}

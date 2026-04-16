/**
 * @file main.c
 * @brief ESP32-C3 NFC Tool 入口
 *
 * 启动顺序：NVS → LittleFS → Wi-Fi AP → Captive DNS → PN532 → HTTP Server
 */

#include "captive_dns.h"
#include "config.h"
#include "dump_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "keys_store.h"
#include "nvs_flash.h"
#include "pn532.h"
#include "wifi_ap.h"

static const char* TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "esp32-nfc-tool v%s starting", APP_VERSION);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    dump_store_init();
    keys_store_init();

    wifi_ap_start();
    captive_dns_start();

    if (pn532_init() != ESP_OK) {
        ESP_LOGW(TAG, "PN532 init failed (检查接线/拨码开关是否切到 SPI 模式)");
    } else {
        uint32_t fw = pn532_get_firmware_version();
        ESP_LOGI(TAG, "PN532 firmware: 0x%08lx", (unsigned long)fw);
    }

    http_server_start();

    ESP_LOGI(TAG, "ready, connect to AP \"%s\" then open http://nfc.local/", WIFI_AP_SSID);
}

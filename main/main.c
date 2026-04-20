#include "captive_dns.h"
#include "config.h"
#include "dump_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "http_server.h"
#include "keys_store.h"
#include "nfc_lock.h"
#include "nfc_monitor.h"
#include "nvs_flash.h"
#include "pn532.h"
#include "wifi_ap.h"

static const char* TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "esp32-nfc-tool v%s starting", APP_VERSION);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs partition corrupted, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    dump_store_init();
    keys_store_init();

    wifi_ap_start();
    captive_dns_start();

    // 锁先于 pn532_init 创建：即便 pn532 不可用，HTTP handler 也走同一路径用 nfc_acquire 拒绝并发
    nfc_lock_init();
    if (pn532_init() != ESP_OK) {
        ESP_LOGW(TAG, "pn532 init failed, check wiring and dip switch");
    } else {
        nfc_monitor_start();
    }

    http_server_start();

    ESP_LOGI(TAG, "ready, ap=\"%s\" url=http://nfc.local/", CONFIG_ESP_WIFI_SSID);
}

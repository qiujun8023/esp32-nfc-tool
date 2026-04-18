#include "wifi_ap.h"

#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "http_server.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"

static const char* TAG = "wifi_ap";

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* e = (wifi_event_ap_staconnected_t*)event_data;
        ESP_LOGI(TAG, "sta connected, mac=" MACSTR, MAC2STR(e->mac));
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* e = (wifi_event_ap_stadisconnected_t*)event_data;
        ESP_LOGI(TAG, "sta disconnected, mac=" MACSTR, MAC2STR(e->mac));
        http_server_portal_clear_all();
    }
}

// DHCP option 114 (RFC 8910) captive portal URI,iOS 14+/Android 11+ 据此自动弹窗
static char s_portal_uri[] = "http://192.168.4.1/";

static void configure_dhcps_captive(esp_netif_t* ap_netif) {
    // DHCPS 启动后无法改选项,必须先停
    esp_netif_dhcps_stop(ap_netif);

    // 显式把自己设为主 DNS,确保 captive_dns 能截获所有域名查询
    esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4 };
    IP4_ADDR(&dns.ip.u_addr.ip4, 192, 168, 4, 1);
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);

    // 默认不下发 DNS option,这里打开
    uint8_t offer_dns = 1;
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));

    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_CAPTIVEPORTAL_URI,
                           s_portal_uri, sizeof(s_portal_uri) - 1);

    esp_netif_dhcps_start(ap_netif);
}

void wifi_ap_start(void) {
    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();
    configure_dhcps_captive(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len       = strlen(CONFIG_ESP_WIFI_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .max_connection = CONFIG_ESP_MAX_STA_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg        = { .required = true },
        },
    };
    strlcpy((char*)wifi_config.ap.ssid,     CONFIG_ESP_WIFI_SSID,     sizeof(wifi_config.ap.ssid));
    strlcpy((char*)wifi_config.ap.password, CONFIG_ESP_WIFI_PASSWORD, sizeof(wifi_config.ap.password));
    if (strlen(CONFIG_ESP_WIFI_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set("ESP32 NFC Tool");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    ESP_LOGI(TAG, "softap up, ssid=%s channel=%d", CONFIG_ESP_WIFI_SSID, WIFI_AP_CHANNEL);
}

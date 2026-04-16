#pragma once

#define APP_VERSION "0.1.0"

// Wi-Fi AP
#define WIFI_AP_SSID "esp32-nfc-tool"
#define WIFI_AP_PASS ""
#define WIFI_AP_CHANNEL 6
#define WIFI_AP_MAX_CONN 4

// PN532 SPI
// 拨码开关切到 SPI 模式，接线按下表（可根据实际修改引脚号）
#define PN532_SPI_HOST SPI2_HOST
#define PN532_SPI_SCK  4
#define PN532_SPI_MISO 5
#define PN532_SPI_MOSI 6
#define PN532_SPI_SS   7
#define PN532_SPI_FREQ (2 * 1000 * 1000)  // 2 MHz

// LittleFS
#define LFS_BASE "/lfs"
#define LFS_PARTITION_LABEL "storage"
#define DUMPS_DIR LFS_BASE "/dumps"

// NVS namespace
#define NVS_NS_KEYS "nfc_keys"

// mDNS
#define MDNS_HOSTNAME "nfc"

#pragma once

#define APP_VERSION "0.1.0"

// SSID / 密码 / 最大连接数由 menuconfig → "NFC Tool Configuration" 提供
#define WIFI_AP_CHANNEL 6

// PN532 拨码开关需切到 SPI 模式
#define PN532_SPI_HOST SPI2_HOST
#define PN532_SPI_SCK  4
#define PN532_SPI_MISO 5
#define PN532_SPI_MOSI 6
#define PN532_SPI_SS   7
#define PN532_SPI_FREQ (2 * 1000 * 1000)

#define LFS_BASE "/lfs"
#define LFS_PARTITION_LABEL "storage"
#define DUMPS_DIR LFS_BASE "/dumps"

#define NVS_NS_KEYS "nfc_keys"

#define MDNS_HOSTNAME "nfc"

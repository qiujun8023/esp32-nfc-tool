#pragma once

#define APP_VERSION "0.1.0"

// SSID / 密码 / 最大连接数由 menuconfig → "NFC Tool Configuration" 提供
#define WIFI_AP_CHANNEL 6

// PN532 拨码开关需切到 SPI 模式
// ESP32-C3 SuperMini 左侧 8 针（5V/G/3V3/GPIO4/3/2/1/0）占 4 根 GPIO；
// 跳过 GPIO2（strapping 引脚，启动时必须 HIGH，不能给高阻输入 MISO）
#define PN532_SPI_HOST SPI2_HOST
#define PN532_SPI_SCK  0
#define PN532_SPI_MISO 1
#define PN532_SPI_MOSI 3
#define PN532_SPI_SS   4
#define PN532_SPI_FREQ (2 * 1000 * 1000)

#define LFS_BASE "/lfs"
#define LFS_PARTITION_LABEL "storage"
#define DUMPS_DIR LFS_BASE "/dumps"

#define NVS_NS_KEYS "nfc_keys"

#define MDNS_HOSTNAME "nfc"

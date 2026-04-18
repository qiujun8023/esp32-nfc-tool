# esp32-nfc-tool

ESP32-C3 + PN532 的便携 NFC 工具：开 Wi-Fi AP，浏览器操作，复制 IC 卡（Mifare Classic / NTAG）。

## 功能

- 扫描 ISO14443A 卡片（Mifare Classic 1K/4K、NTAG21x、Ultralight）
- 魔术卡检测（Gen1a / Gen2 CUID）
- Mifare Classic **字典攻击** + 完整 dump（写回时逐块校验）
- NTAG 全页读取 / 写回
- NDEF 解析与写入（URI、Text 记录）
- 卡库管理：列表 / 改名 / 下载 .bin / 导入 / 写回 / 删除
- Mifare Classic 访问位解码（权限可视化）
- 自定义密钥库（NVS 持久化）+ 50 条内置默认密钥
- WebSocket 实时进度推送（扇区级别）
- Captive Portal：连上 AP 自动弹窗
- Web OTA 固件更新

**不支持**：Nested / Darkside 攻击、125kHz LF（EM4100 等）、SD 卡。

## 硬件接线

| ESP32-C3 | PN532（SPI 模式） |
|----------|-------------------|
| 3V3      | VCC               |
| GND      | GND               |
| GPIO4    | SCK               |
| GPIO5    | MISO              |
| GPIO6    | MOSI              |
| GPIO7    | SS (CS)           |

PN532 模块上的拨码开关切到 SPI 模式（请看模块丝印）。

## 构建 & 烧录

需要 ESP-IDF v5.0+：

```bash
cd esp32-nfc-tool
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor
```

首次构建会自动从 IDF Component Manager 拉取 `joltwallet/littlefs` 和 `espressif/mdns`。

## 使用

1. 烧录后串口能看到 `PN532 firmware: 0x...`，说明硬件 OK
2. 手机/电脑连 Wi-Fi `ESP32-NFC-Tool`（开放网络，无密码）
3. 浏览器打开 `http://192.168.4.1/` 或 `http://nfc.local/`（多数系统会自动弹）
4. 「扫描」页贴卡 → 看到 UID → 选「读取 Dump」或 NTAG 的「读取 NDEF」
5. 读取期间扇区/进度条实时更新；存入卡库后可改名 / 下载 / 写回 / 删除

## 引脚改动

SPI 引脚改 `main/config.h`：

```c
#define PN532_SPI_SCK  4
#define PN532_SPI_MISO 5
#define PN532_SPI_MOSI 6
#define PN532_SPI_SS   7
```

Wi-Fi SSID / 密码 / 最大连接数通过 `idf.py menuconfig → NFC Tool Configuration` 调整。

## 目录结构

```
├── main/
│   ├── main.c             入口
│   ├── config.h           全局常量（SPI 引脚 / 分区 / NVS 命名空间 …）
│   ├── Kconfig.projbuild  menuconfig 条目（SSID / 密码 / 最大连接数）
│   ├── nfc/               PN532 驱动 + Mifare Classic + NTAG + NDEF
│   ├── storage/           LittleFS dump 库 + NVS 密钥库
│   ├── net/               Wi-Fi AP + Captive DNS + HTTP/WS API + OTA
│   └── html/              前端资源（嵌入到固件）
├── partitions.csv         分区表（双 OTA）
└── sdkconfig.defaults     默认构建配置
```

## 致谢 / 参考

- [Senape3000/nfc-tool_ESP32](https://github.com/Senape3000/nfc-tool_ESP32) — 整体架构借鉴
- [dkyazzentwatwa/cypher-pn532](https://github.com/dkyazzentwatwa/cypher-pn532) — 字典攻击算法和默认密钥列表
- [lucafaccin/esp-pn532](https://github.com/lucafaccin/esp-pn532) — PN532 帧格式参考

## License

[MIT](LICENSE)

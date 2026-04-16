/**
 * @file pn532.c
 * @brief PN532 NFC 控制器 SPI 驱动
 *
 * 基于 Garag/esp-idf-pn532 参考实现，关键配置：
 *   - 全双工 + SPI_DEVICE_BIT_LSBFIRST（ESP32-C3 半双工不支持同时 TX+RX）
 *   - opcode 作为 tx_buffer 首字节发送，读操作从 rx_buffer[1] 取数据
 *   - 手动 CS 控制（pre_cb / post_cb 回调）
 *   - SPI Mode 0（CPOL=0, CPHA=0）
 *
 * SPI 协议：
 *   - 写数据：TX=[0x01, 帧数据...]，忽略 RX
 *   - 读状态：TX=[0x02, 0x00]，RX=[xx, status]，bit0=1 就绪
 *   - 读数据：TX=[0x03, 0x00...]，RX=[xx, 帧数据...]
 *
 * 帧格式：00 00 FF LEN LCS TFI DATA... DCS 00
 * ACK：00 00 FF 00 FF 00
 */

#include "pn532.h"

#include <string.h>

#include "config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "pn532";

/* PN532 帧常量 */
#define PN532_PREAMBLE      0x00
#define PN532_STARTCODE1    0x00
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00
#define PN532_HOSTTOPN532   0xD4
#define PN532_PN532TOHOST   0xD5

/* SPI 命令字节（opcode） */
#define SPI_OP_DATA_WRITE   0x01
#define SPI_OP_STATUS_READ  0x02
#define SPI_OP_DATA_READ    0x03

/* PN532 命令码 */
#define CMD_GET_FIRMWARE_VERSION  0x02
#define CMD_SAM_CONFIGURATION     0x14
#define CMD_IN_LIST_PASSIVE       0x4A
#define CMD_IN_DATA_EXCHANGE      0x40

static spi_device_handle_t s_spi  = NULL;
static SemaphoreHandle_t   s_lock = NULL;

#define PN532_LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define PN532_UNLOCK() xSemaphoreGive(s_lock)

static const uint8_t ACK_FRAME[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

/* -------- CS 控制（pre/post callback） -------- */

static void IRAM_ATTR spi_pre_cb(spi_transaction_t* t) {
    (void)t;
    gpio_set_level(PN532_SPI_SS, 0);
    esp_rom_delay_us(100);  // PN532 需要 CS 拉低后等待 ≥2µs，这里留 100µs 余量
}

static void IRAM_ATTR spi_post_cb(spi_transaction_t* t) {
    (void)t;
    gpio_set_level(PN532_SPI_SS, 1);
}

/* -------- 调试 -------- */

static void hexdump(const char* label, const uint8_t* d, size_t n) {
    char buf[3 * 32 + 1];
    size_t k = (n > 32) ? 32 : n;
    for (size_t i = 0; i < k; i++) snprintf(buf + i * 3, 4, "%02x ", d[i]);
    buf[k * 3] = 0;
    ESP_LOGI(TAG, "%s[%d]: %s", label, (int)n, buf);
}

/* -------- SPI 传输（全双工，opcode 作为 tx 首字节） -------- */

/**
 * @brief 写数据到 PN532（TX=[0x01, 帧数据...]）
 */
static esp_err_t spi_write_data(const uint8_t* data, size_t len) {
    uint8_t buf[1 + 8 + 256];
    if (len > sizeof(buf) - 1) return ESP_ERR_INVALID_SIZE;
    buf[0] = SPI_OP_DATA_WRITE;
    memcpy(buf + 1, data, len);
    spi_transaction_t t = {
        .length    = (len + 1) * 8,
        .tx_buffer = buf,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

/**
 * @brief 读 PN532 状态（TX=[0x02,0x00]，RX=[xx,status]）
 * rx[0] 是 opcode 发送期间的垃圾，状态在 rx[1]
 */
static bool spi_is_ready(void) {
    uint8_t tx[2] = {SPI_OP_STATUS_READ, 0x00};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length    = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    if (spi_device_polling_transmit(s_spi, &t) != ESP_OK) return false;
    return (rx[1] & 0x01) != 0;
}

/**
 * @brief 从 PN532 读数据（TX=[0x03,0x00...]，RX=[xx,帧数据...]）
 * rx[0] 是垃圾，实际帧数据从 rx[1] 开始
 */
static esp_err_t spi_read_data(uint8_t* data, size_t len) {
    uint8_t tx[1 + 263];
    uint8_t rx[1 + 263];
    if (len > sizeof(tx) - 1) return ESP_ERR_INVALID_SIZE;
    memset(tx, 0, 1 + len);
    tx[0] = SPI_OP_DATA_READ;
    spi_transaction_t t = {
        .length    = (len + 1) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err == ESP_OK) {
        memcpy(data, rx + 1, len);  // 跳过第 1 字节（opcode 期间的垃圾）
    }
    return err;
}

/* -------- PN532 帧层 -------- */

/**
 * @brief 构造并发送命令帧
 */
static esp_err_t send_command(const uint8_t* cmd, uint8_t cmd_len) {
    if (cmd_len > 254) return ESP_ERR_INVALID_ARG;

    uint8_t frame[8 + 256];
    uint8_t i = 0;
    frame[i++] = PN532_PREAMBLE;
    frame[i++] = PN532_STARTCODE1;
    frame[i++] = PN532_STARTCODE2;
    uint8_t len = cmd_len + 1;  // +TFI
    frame[i++] = len;
    frame[i++] = (uint8_t)(~len + 1);  // LCS
    frame[i++] = PN532_HOSTTOPN532;     // TFI

    uint8_t sum = PN532_HOSTTOPN532;
    for (uint8_t k = 0; k < cmd_len; k++) {
        frame[i++] = cmd[k];
        sum += cmd[k];
    }
    frame[i++] = (uint8_t)(~sum + 1);  // DCS
    frame[i++] = PN532_POSTAMBLE;

    hexdump("tx", frame, i);
    return spi_write_data(frame, i);
}

/**
 * @brief 轮询等待 PN532 就绪
 */
static esp_err_t wait_ready(uint32_t timeout_ms) {
    uint32_t waited = 0;
    const uint32_t step = 10;
    while (waited < timeout_ms) {
        if (spi_is_ready()) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 读 ACK 帧
 */
static esp_err_t read_ack(uint32_t timeout_ms) {
    esp_err_t err = wait_ready(timeout_ms);
    if (err != ESP_OK) return err;

    uint8_t buf[6] = {0};
    err = spi_read_data(buf, 6);
    if (err != ESP_OK) return err;

    hexdump("ack", buf, 6);
    if (memcmp(buf, ACK_FRAME, 6) != 0) {
        ESP_LOGW(TAG, "ack mismatch");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 读响应帧
 *
 * 先读 preamble(1) + startcode(2) + LEN(1) + LCS(1) = 5 字节
 * 再根据 LEN 读 TFI + payload + DCS + postamble
 */
static esp_err_t read_response(uint8_t expected_cmd, uint8_t* data, uint8_t* data_len,
                               uint32_t timeout_ms) {
    esp_err_t err = wait_ready(timeout_ms);
    if (err != ESP_OK) return err;

    /* 读整个帧（最大 5 + 256 + 2 = 263 字节，但正常不会超过 ~70） */
    uint8_t buf[5 + 258];
    size_t max_read = 5 + (*data_len) + 4;  // header + TFI + CMD + payload + DCS + postamble
    if (max_read > sizeof(buf)) max_read = sizeof(buf);
    err = spi_read_data(buf, max_read);
    if (err != ESP_OK) return err;

    hexdump("rx", buf, max_read > 20 ? 20 : max_read);

    /* 验证帧头 */
    if (buf[0] != 0x00 || buf[1] != 0x00 || buf[2] != 0xFF) {
        ESP_LOGW(TAG, "bad frame header: %02x %02x %02x", buf[0], buf[1], buf[2]);
        return ESP_FAIL;
    }
    uint8_t len = buf[3];
    if ((uint8_t)(len + buf[4]) != 0) {
        ESP_LOGW(TAG, "bad LCS: len=%02x lcs=%02x", len, buf[4]);
        return ESP_FAIL;
    }

    /* 验证 TFI + CMD */
    if (buf[5] != PN532_PN532TOHOST || buf[6] != expected_cmd) {
        ESP_LOGW(TAG, "unexpected: TFI=%02x CMD=%02x (expected %02x)",
                 buf[5], buf[6], expected_cmd);
        return ESP_FAIL;
    }

    /* 提取 payload（跳过 TFI + CMD） */
    uint8_t payload_len = (uint8_t)(len - 2);
    if (payload_len > *data_len) {
        ESP_LOGW(TAG, "rx buf too small: need %d have %d", payload_len, *data_len);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, &buf[7], payload_len);
    *data_len = payload_len;
    return ESP_OK;
}

/**
 * @brief 完整命令事务：发送 → ACK → 响应
 */
static esp_err_t cmd_xfer(const uint8_t* cmd, uint8_t cmd_len, uint8_t* rx, uint8_t* rx_len,
                          uint32_t timeout_ms) {
    esp_err_t err = send_command(cmd, cmd_len);
    if (err != ESP_OK) return err;

    err = read_ack(200);
    if (err != ESP_OK) return err;

    return read_response(cmd[0] + 1, rx, rx_len, timeout_ms);
}

/* -------- 唤醒 -------- */

static void spi_wakeup(void) {
    ESP_LOGI(TAG, "PN532 SPI wakeup (CS low 2ms)...");
    gpio_set_level(PN532_SPI_SS, 0);
    vTaskDelay(pdMS_TO_TICKS(3));   // CS low ≥2ms
    gpio_set_level(PN532_SPI_SS, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); // 等内部振荡器稳定（PN532 datasheet 建议 ≥50ms）
    /* 发一次 dummy status read，让 SPI 接口同步 */
    spi_is_ready();
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* -------- 公开 API -------- */

esp_err_t pn532_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* SS 引脚先配为 GPIO 输出，默认高 */
    gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << PN532_SPI_SS,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(PN532_SPI_SS, 1);

    /* SPI 总线 */
    spi_bus_config_t bus = {
        .mosi_io_num     = PN532_SPI_MOSI,
        .miso_io_num     = PN532_SPI_MISO,
        .sclk_io_num     = PN532_SPI_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 512,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(PN532_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    /* SPI 设备——关键配置
     * 使用全双工模式：ESP32-C3 半双工不支持同一事务中同时 TX+RX。
     * opcode 作为 tx_buffer 首字节发送，读操作从 rx_buffer[1] 取有效数据。
     */
    spi_device_interface_config_t dev = {
        .clock_speed_hz = PN532_SPI_FREQ,
        .mode           = 0,            // CPOL=0, CPHA=0
        .spics_io_num   = -1,           // 手动 CS
        .queue_size     = 1,
        .flags          = SPI_DEVICE_BIT_LSBFIRST,
        .pre_cb         = spi_pre_cb,   // CS low + 100µs delay
        .post_cb        = spi_post_cb,  // CS high
    };
    ESP_ERROR_CHECK(spi_bus_add_device(PN532_SPI_HOST, &dev, &s_spi));

    ESP_LOGI(TAG, "SPI init: SCK=%d MISO=%d MOSI=%d SS=%d freq=%dHz",
             PN532_SPI_SCK, PN532_SPI_MISO, PN532_SPI_MOSI, PN532_SPI_SS, PN532_SPI_FREQ);

    /* 上电等待 */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 唤醒 */
    spi_wakeup();

    /* GetFirmwareVersion 验证通信 */
    uint8_t fw_cmd[] = {CMD_GET_FIRMWARE_VERSION};
    uint8_t rx[8]    = {0};
    uint8_t rx_len   = sizeof(rx);
    esp_err_t err    = cmd_xfer(fw_cmd, 1, rx, &rx_len, 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FW version failed: %s, retry after wakeup", esp_err_to_name(err));
        spi_wakeup();
        rx_len = sizeof(rx);
        err = cmd_xfer(fw_cmd, 1, rx, &rx_len, 1000);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PN532 not responding: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "PN532 firmware: IC=0x%02x Ver=%d.%d Rev=%d",
             rx[0], rx[1], rx[2], rx[3]);

    /* SAMConfiguration: normal mode, no timeout, no IRQ */
    uint8_t sam[] = {CMD_SAM_CONFIGURATION, 0x01, 0x14, 0x01};
    rx_len = sizeof(rx);
    err = cmd_xfer(sam, sizeof(sam), rx, &rx_len, 500);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SAMConfig failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "PN532 ready (SPI)");
    return ESP_OK;
}

uint32_t pn532_get_firmware_version(void) {
    uint8_t cmd[]  = {CMD_GET_FIRMWARE_VERSION};
    uint8_t rx[8]  = {0};
    uint8_t rx_len = sizeof(rx);
    PN532_LOCK();
    esp_err_t err = cmd_xfer(cmd, 1, rx, &rx_len, 200);
    PN532_UNLOCK();
    if (err != ESP_OK || rx_len < 4) return 0;
    return ((uint32_t)rx[0] << 24) | ((uint32_t)rx[1] << 16) |
           ((uint32_t)rx[2] << 8) | rx[3];
}

static pn532_card_type_t classify(uint8_t sak) {
    switch (sak) {
        case 0x08: return PN532_CARD_MIFARE_CLASSIC_1K;
        case 0x18: return PN532_CARD_MIFARE_CLASSIC_4K;
        case 0x00: return PN532_CARD_MIFARE_ULTRALIGHT;
        default:   return PN532_CARD_UNKNOWN;
    }
}

esp_err_t pn532_read_passive_target(pn532_target_t* out, uint32_t timeout_ms) {
    uint8_t cmd[]  = {CMD_IN_LIST_PASSIVE, 0x01, 0x00};
    uint8_t rx[32] = {0};
    uint8_t rx_len = sizeof(rx);
    PN532_LOCK();
    esp_err_t err = cmd_xfer(cmd, sizeof(cmd), rx, &rx_len, timeout_ms);
    PN532_UNLOCK();
    if (err != ESP_OK) return err;
    if (rx_len < 6 || rx[0] != 1) return ESP_ERR_NOT_FOUND;
    out->atqa    = ((uint16_t)rx[2] << 8) | rx[3];
    out->sak     = rx[4];
    out->uid_len = rx[5];
    if (out->uid_len > sizeof(out->uid)) return ESP_ERR_INVALID_RESPONSE;
    memcpy(out->uid, &rx[6], out->uid_len);
    out->type = classify(out->sak);
    return ESP_OK;
}

esp_err_t pn532_in_data_exchange(const uint8_t* tx, uint8_t tx_len,
                                 uint8_t* rx, uint8_t* rx_len,
                                 uint32_t timeout_ms) {
    if (tx_len > 250) return ESP_ERR_INVALID_ARG;
    uint8_t cmd[2 + 256];
    cmd[0] = CMD_IN_DATA_EXCHANGE;
    cmd[1] = 0x01;  // Tg=1
    memcpy(cmd + 2, tx, tx_len);

    uint8_t buf[64];
    uint8_t buf_len = sizeof(buf);
    PN532_LOCK();
    esp_err_t err = cmd_xfer(cmd, tx_len + 2, buf, &buf_len, timeout_ms);
    PN532_UNLOCK();
    if (err != ESP_OK) return err;
    if (buf_len < 1 || buf[0] != 0x00) return ESP_FAIL;
    uint8_t payload = buf_len - 1;
    if (payload > *rx_len) return ESP_ERR_INVALID_SIZE;
    memcpy(rx, buf + 1, payload);
    *rx_len = payload;
    return ESP_OK;
}

/*
 * PN532 SPI 驱动。关键点:
 *  - ESP32-C3 半双工模式不支持同事务 TX+RX,必须用全双工 + LSB-first
 *  - PN532 SPI 前端要求 LSB-first (和 HSU 不同),opcode 也是这个字序
 *  - opcode 作为 tx_buffer 首字节;读时丢弃 rx_buffer[0],有效数据从 [1] 开始
 *  - CS 手动控制,拉低后必须等 >= 2µs 才发首字节(这里留了 100µs)
 *
 * SPI opcode:
 *  - 0x01 write: TX=[0x01, 帧...]
 *  - 0x02 status: TX=[0x02,0x00] RX=[xx,status],status bit0=1 表示就绪
 *  - 0x03 read: TX=[0x03,0...] RX=[xx, 帧...]
 *
 * 正常帧: 00 00 FF LEN LCS TFI DATA... DCS 00
 * ACK:   00 00 FF 00 FF 00
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

#define PN532_PREAMBLE      0x00
#define PN532_STARTCODE1    0x00
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00
#define PN532_HOSTTOPN532   0xD4
#define PN532_PN532TOHOST   0xD5

#define SPI_OP_DATA_WRITE   0x01
#define SPI_OP_STATUS_READ  0x02
#define SPI_OP_DATA_READ    0x03

// preamble(1)+startcode(2)+len(1)+lcs(1)+TFI(1)+DATA(<=254)+DCS(1)+postamble(1)=262
#define PN532_MAX_FRAME     263
#define SPI_XFER_BUF_SIZE   (1 + PN532_MAX_FRAME)

#define CMD_GET_FIRMWARE_VERSION  0x02
#define CMD_SAM_CONFIGURATION     0x14
#define CMD_IN_LIST_PASSIVE       0x4A
#define CMD_IN_DATA_EXCHANGE      0x40

static spi_device_handle_t s_spi  = NULL;
static SemaphoreHandle_t   s_lock = NULL;

#define PN532_LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define PN532_UNLOCK() xSemaphoreGive(s_lock)

static const uint8_t ACK_FRAME[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

static void IRAM_ATTR spi_pre_cb(spi_transaction_t* t) {
    (void)t;
    gpio_set_level(PN532_SPI_SS, 0);
    // PN532 要求 CS 拉低后 >= 2µs 才能开始传输, 100µs 留足余量
    esp_rom_delay_us(100);
}

static void IRAM_ATTR spi_post_cb(spi_transaction_t* t) {
    (void)t;
    gpio_set_level(PN532_SPI_SS, 1);
}

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

// rx[0] 是发 opcode 时采到的脏字节,状态字节在 rx[1]; bit0=1 表示 PN532 有数据可读
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

// rx[0] 为 opcode 采到的脏字节,有效帧从 rx[1] 开始
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
        memcpy(data, rx + 1, len);
    }
    return err;
}

static esp_err_t send_command(const uint8_t* cmd, uint8_t cmd_len) {
    if (cmd_len > 254) return ESP_ERR_INVALID_ARG;

    uint8_t frame[8 + 256];
    uint8_t i = 0;
    frame[i++] = PN532_PREAMBLE;
    frame[i++] = PN532_STARTCODE1;
    frame[i++] = PN532_STARTCODE2;
    uint8_t len = cmd_len + 1;  // LEN 包含 TFI
    frame[i++] = len;
    frame[i++] = (uint8_t)(~len + 1);  // LCS = -LEN (两者之和的低 8 位 = 0)
    frame[i++] = PN532_HOSTTOPN532;

    uint8_t sum = PN532_HOSTTOPN532;
    for (uint8_t k = 0; k < cmd_len; k++) {
        frame[i++] = cmd[k];
        sum += cmd[k];
    }
    frame[i++] = (uint8_t)(~sum + 1);  // DCS = -sum(TFI..DATA)
    frame[i++] = PN532_POSTAMBLE;

    return spi_write_data(frame, i);
}

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

static esp_err_t read_ack(uint32_t timeout_ms) {
    esp_err_t err = wait_ready(timeout_ms);
    if (err != ESP_OK) return err;

    uint8_t buf[6] = {0};
    err = spi_read_data(buf, 6);
    if (err != ESP_OK) return err;

    if (memcmp(buf, ACK_FRAME, 6) != 0) {
        ESP_LOGW(TAG, "ack mismatch");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 读整帧: header(5) + TFI + CMD + payload + DCS + postamble
static esp_err_t read_response(uint8_t expected_cmd, uint8_t* data, uint8_t* data_len,
                               uint32_t timeout_ms) {
    esp_err_t err = wait_ready(timeout_ms);
    if (err != ESP_OK) return err;

    uint8_t buf[5 + 258];
    size_t max_read = 5 + (*data_len) + 4;
    if (max_read > sizeof(buf)) max_read = sizeof(buf);
    err = spi_read_data(buf, max_read);
    if (err != ESP_OK) return err;

    if (buf[0] != 0x00 || buf[1] != 0x00 || buf[2] != 0xFF) {
        ESP_LOGW(TAG, "bad frame header: %02x %02x %02x", buf[0], buf[1], buf[2]);
        return ESP_FAIL;
    }
    uint8_t len = buf[3];
    // LEN + LCS 的低 8 位必须为 0,否则帧损坏
    if ((uint8_t)(len + buf[4]) != 0) {
        ESP_LOGW(TAG, "bad lcs: len=%02x lcs=%02x", len, buf[4]);
        return ESP_FAIL;
    }

    if (buf[5] != PN532_PN532TOHOST || buf[6] != expected_cmd) {
        ESP_LOGW(TAG, "unexpected: tfi=%02x cmd=%02x (expected %02x)",
                 buf[5], buf[6], expected_cmd);
        return ESP_FAIL;
    }

    uint8_t payload_len = (uint8_t)(len - 2);
    if (payload_len > *data_len) {
        ESP_LOGW(TAG, "rx buf too small: need %d have %d", payload_len, *data_len);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, &buf[7], payload_len);
    *data_len = payload_len;
    return ESP_OK;
}

// 命令事务: send -> ACK -> response
static esp_err_t cmd_xfer(const uint8_t* cmd, uint8_t cmd_len, uint8_t* rx, uint8_t* rx_len,
                          uint32_t timeout_ms) {
    esp_err_t err = send_command(cmd, cmd_len);
    if (err != ESP_OK) return err;

    err = read_ack(200);
    if (err != ESP_OK) return err;

    return read_response(cmd[0] + 1, rx, rx_len, timeout_ms);
}

// PN532 上电/唤醒: CS 低 >= 2ms 触发 SPI 模式识别,之后等振荡器稳定 (datasheet 建议 >= 50ms)
static void spi_wakeup(void) {
    gpio_set_level(PN532_SPI_SS, 0);
    vTaskDelay(pdMS_TO_TICKS(3));
    gpio_set_level(PN532_SPI_SS, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    // 一次 dummy status read 让 SPI FSM 进入空闲态
    spi_is_ready();
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t pn532_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    // 手动控制 CS,所以 SS 按普通 GPIO 初始化为高
    gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << PN532_SPI_SS,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(PN532_SPI_SS, 1);

    spi_bus_config_t bus = {
        .mosi_io_num     = PN532_SPI_MOSI,
        .miso_io_num     = PN532_SPI_MISO,
        .sclk_io_num     = PN532_SPI_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 512,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(PN532_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    // LSB-first 是 PN532 SPI 前端的硬性要求; spics_io_num=-1 表示 CS 在 pre/post cb 中手动拉
    spi_device_interface_config_t dev = {
        .clock_speed_hz = PN532_SPI_FREQ,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 1,
        .flags          = SPI_DEVICE_BIT_LSBFIRST,
        .pre_cb         = spi_pre_cb,
        .post_cb        = spi_post_cb,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(PN532_SPI_HOST, &dev, &s_spi));

    ESP_LOGI(TAG, "spi init sck=%d miso=%d mosi=%d ss=%d freq=%dhz",
             PN532_SPI_SCK, PN532_SPI_MISO, PN532_SPI_MOSI, PN532_SPI_SS, PN532_SPI_FREQ);

    // 给模块一点上电时间再发命令,避免首次 GET_FW_VERSION 失败
    vTaskDelay(pdMS_TO_TICKS(500));

    spi_wakeup();

    uint8_t fw_cmd[] = {CMD_GET_FIRMWARE_VERSION};
    uint8_t rx[8]    = {0};
    uint8_t rx_len   = sizeof(rx);
    esp_err_t err    = cmd_xfer(fw_cmd, 1, rx, &rx_len, 1000);
    if (err != ESP_OK) {
        // 首次 wakeup 偶尔不稳,补一次 wakeup 再试
        ESP_LOGW(TAG, "fw version failed: %s, retry after wakeup", esp_err_to_name(err));
        spi_wakeup();
        rx_len = sizeof(rx);
        err = cmd_xfer(fw_cmd, 1, rx, &rx_len, 1000);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pn532 not responding: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "pn532 firmware ic=0x%02x ver=%d.%d rev=%d",
             rx[0], rx[1], rx[2], rx[3]);

    // SAMConfiguration: Normal mode, no timeout, IRQ disabled (0x14 = 20 * 50ms 超时但 normal 模式下不生效)
    uint8_t sam[] = {CMD_SAM_CONFIGURATION, 0x01, 0x14, 0x01};
    rx_len = sizeof(rx);
    err = cmd_xfer(sam, sizeof(sam), rx, &rx_len, 500);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "samconfig failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "pn532 ready");
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
    cmd[1] = 0x01;  // 目标编号固定 1 (单卡场景)
    memcpy(cmd + 2, tx, tx_len);

    uint8_t buf[64];
    uint8_t buf_len = sizeof(buf);
    PN532_LOCK();
    esp_err_t err = cmd_xfer(cmd, tx_len + 2, buf, &buf_len, timeout_ms);
    PN532_UNLOCK();
    if (err != ESP_OK) return err;
    // buf[0] 是 PN532 的 status byte, 非 0 表示 exchange 失败
    if (buf_len < 1 || buf[0] != 0x00) return ESP_FAIL;
    uint8_t payload = buf_len - 1;
    if (payload > *rx_len) return ESP_ERR_INVALID_SIZE;
    memcpy(rx, buf + 1, payload);
    *rx_len = payload;
    return ESP_OK;
}

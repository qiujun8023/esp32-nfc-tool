/**
 * @file ntag.c
 * @brief NTAG21x（NFC Forum Type 2）读写
 */

#include "ntag.h"

#include <string.h>

#include "esp_log.h"

static const char* TAG = "ntag";

#define NTAG_CMD_READ 0x30
#define NTAG_CMD_WRITE 0xA2
#define NTAG_CMD_GET_VERSION 0x60

ntag_type_t ntag_detect_type(void) {
    uint8_t tx[]   = {NTAG_CMD_GET_VERSION};
    uint8_t rx[16] = {0};
    uint8_t rx_len = sizeof(rx);
    if (pn532_in_data_exchange(tx, 1, rx, &rx_len, 200) != ESP_OK || rx_len < 8) {
        return NTAG_UNKNOWN;
    }
    // rx[6] 是 storage size code
    switch (rx[6]) {
        case 0x0F:
            return NTAG_213;
        case 0x11:
            return NTAG_215;
        case 0x13:
            return NTAG_216;
        default:
            return NTAG_UNKNOWN;
    }
}

esp_err_t ntag_read_page(uint8_t page, uint8_t out[NTAG_PAGE_SIZE]) {
    uint8_t tx[]   = {NTAG_CMD_READ, page};
    uint8_t rx[32] = {0};
    uint8_t rx_len = sizeof(rx);
    esp_err_t err  = pn532_in_data_exchange(tx, sizeof(tx), rx, &rx_len, 200);
    if (err != ESP_OK || rx_len < NTAG_PAGE_SIZE) return ESP_FAIL;
    memcpy(out, rx, NTAG_PAGE_SIZE);
    return ESP_OK;
}

esp_err_t ntag_write_page(uint8_t page, const uint8_t in[NTAG_PAGE_SIZE]) {
    uint8_t tx[2 + NTAG_PAGE_SIZE];
    tx[0] = NTAG_CMD_WRITE;
    tx[1] = page;
    memcpy(tx + 2, in, NTAG_PAGE_SIZE);
    uint8_t rx[8];
    uint8_t rx_len = sizeof(rx);
    return pn532_in_data_exchange(tx, sizeof(tx), rx, &rx_len, 200);
}

static uint16_t pages_for(ntag_type_t t) {
    switch (t) {
        case NTAG_213:
            return 45;
        case NTAG_215:
            return 135;
        case NTAG_216:
            return 231;
        default:
            return 16;  // Ultralight 兜底
    }
}

esp_err_t ntag_full_read(const pn532_target_t* tgt, ntag_dump_t* dump) {
    (void)tgt;
    memset(dump, 0, sizeof(*dump));
    dump->type        = ntag_detect_type();
    dump->total_pages = pages_for(dump->type);
    for (uint16_t p = 0; p < dump->total_pages; p++) {
        if (ntag_read_page(p, dump->pages[p]) == ESP_OK) {
            dump->page_read[p] = true;
        }
    }
    ESP_LOGI(TAG, "ntag dump done, type=%d pages=%d", dump->type, dump->total_pages);
    return ESP_OK;
}

esp_err_t ntag_full_write(const ntag_dump_t* dump) {
    // 跳过 page 0/1（UID 只读）、page 2（lock bytes）、page 3（CC）通常也不写
    for (uint16_t p = 4; p < dump->total_pages; p++) {
        if (!dump->page_read[p]) continue;
        ntag_write_page((uint8_t)p, dump->pages[p]);
    }
    return ESP_OK;
}

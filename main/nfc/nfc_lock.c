#include "nfc_lock.h"

static SemaphoreHandle_t s_busy = NULL;

esp_err_t nfc_lock_init(void) {
    if (s_busy) return ESP_OK;
    s_busy = xSemaphoreCreateBinary();
    if (!s_busy) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_busy);
    return ESP_OK;
}

BaseType_t nfc_acquire(TickType_t wait_ticks) {
    if (!s_busy) return pdFALSE;
    return xSemaphoreTake(s_busy, wait_ticks);
}

void nfc_release(void) {
    if (!s_busy) return;
    xSemaphoreGive(s_busy);
}

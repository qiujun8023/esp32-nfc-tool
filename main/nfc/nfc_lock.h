#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * 全局唯一的 NFC 访问锁。HTTP 任务和后台监控任务都通过它互斥访问 PN532。
 * 用 binary semaphore 而非 mutex 是因为不同任务 take/give，不符合 mutex 所有权协议。
 * nfc_lock_init 必须在任何 acquire/release 之前调用一次（main 启动早期）。
 */
esp_err_t  nfc_lock_init(void);
BaseType_t nfc_acquire(TickType_t wait_ticks);
void       nfc_release(void);

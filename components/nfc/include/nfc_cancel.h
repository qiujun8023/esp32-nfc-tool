#pragma once

#include <stdbool.h>

/**
 * @brief 全局取消标志，供长任务（mfc/ntag full read/write）主动检查。
 *
 * API 端调用 request()，任务侧每扇区/页后调用 pending() 并提前返回。
 * 任务启动前应调 clear()。
 */
void nfc_cancel_request(void);
void nfc_cancel_clear(void);
bool nfc_cancel_pending(void);

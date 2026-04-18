#pragma once

#include <stdbool.h>

/*
 * 全局取消标志，供 mfc/ntag 长任务主动轮询。
 * API 层 request()，任务每扇区/页后 pending() 提前返回，任务启动前先 clear()。
 */
void nfc_cancel_request(void);
void nfc_cancel_clear(void);
bool nfc_cancel_pending(void);

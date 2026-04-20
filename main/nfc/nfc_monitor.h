#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t nfc_monitor_start(void);

/*
 * 通知 monitor "卡已被 HTTP handler 确认存在"。
 * 调用后下一轮迭代会跳过 SCANNING 直接进入 PRESENT 状态，
 * 不再发 card_in（前端通过 HTTP 响应已拿到卡片信息）。
 * uid_len=0 时仅做静默状态切换，没有 UID 比对（用于不带 UID 上下文的场景）。
 */
void nfc_monitor_mark_present(const uint8_t* uid, uint8_t uid_len);

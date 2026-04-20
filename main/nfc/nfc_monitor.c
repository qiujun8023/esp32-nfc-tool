/*
 * 后台 NFC 监控任务，实现"贴卡自动识别 / 离卡自动清除"。
 *
 * 状态机：
 *   SCANNING ──(检测到卡)──► PRESENT(无 magic)
 *   PRESENT  ──(首轮:补 magic)──► PRESENT(已就绪)
 *   PRESENT  ──(连续 2 次 ping 失败)──► SCANNING
 *   任意态   ──(HTTP 已确认贴卡)──► PRESENT(已就绪,静默)
 *
 * 关键设计：SCANNING 命中后立刻放锁并发不带 magic 的 card_in（用户瞬间看到卡），
 * magic 检测推到 PRESENT 第一轮再做并以 card_meta 推送（mfc_detect_magic 自身 ~1.2s）。
 * 这一轮内 HTTP handler 仍可能拿不到锁，故 acquire 超时设为 1500ms 覆盖。
 *
 * 并发：监控任务优先级 4，低于 httpd 默认 5，HTTP 操作可抢占监控的扫描窗口。
 */

#include "nfc_monitor.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hex_util.h"
#include "mifare_classic.h"
#include "nfc_lock.h"
#include "pn532.h"
#include "ws_progress.h"

static const char* TAG = "nfc_mon";

typedef enum { STATE_SCANNING, STATE_PRESENT } monitor_state_t;

// 当前会话的卡信息；mark_present 与 monitor task 之间的共享需用 atomic 标志同步切换
static uint8_t           s_uid[10];
static uint8_t           s_uid_len;
static pn532_card_type_t s_card_type;
static bool              s_magic_done;

// HTTP handler 设此标志后 monitor 下轮迭代会切到 PRESENT(已就绪)
static atomic_bool s_mark_present = false;
static uint8_t     s_mark_uid[10];
static uint8_t     s_mark_uid_len;

void nfc_monitor_mark_present(const uint8_t* uid, uint8_t uid_len) {
    if (uid && uid_len > 0 && uid_len <= sizeof(s_mark_uid)) {
        memcpy(s_mark_uid, uid, uid_len);
        s_mark_uid_len = uid_len;
    } else {
        s_mark_uid_len = 0;
    }
    atomic_store(&s_mark_present, true);
}

static void emit_card_in_no_magic(const pn532_target_t* tgt) {
    char uid_hex[24];
    hex_encode_upper(tgt->uid, tgt->uid_len, uid_hex);
    ws_card_in(uid_hex, tgt->type, pn532_card_type_str(tgt->type), NULL);
    ESP_LOGI(TAG, "card in uid=%s type=%d", uid_hex, tgt->type);
}

static void monitor_task(void* arg) {
    (void)arg;
    monitor_state_t state      = STATE_SCANNING;
    uint8_t         fail_count = 0;

    while (1) {
        // HTTP 端确认贴卡：直接静默切到 PRESENT，跳过本轮扫描
        if (atomic_exchange(&s_mark_present, false)) {
            state          = STATE_PRESENT;
            fail_count     = 0;
            s_uid_len      = s_mark_uid_len;
            if (s_uid_len) memcpy(s_uid, s_mark_uid, s_uid_len);
            // HTTP 端（如 handle_scan）已经做过 magic 检测，不需要 monitor 再做
            s_magic_done = true;
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        nfc_acquire(portMAX_DELAY);

        if (state == STATE_SCANNING) {
            pn532_target_t tgt;
            esp_err_t      r = pn532_read_passive_target(&tgt, 300);
            nfc_release();

            if (r == ESP_OK) {
                emit_card_in_no_magic(&tgt);
                memcpy(s_uid, tgt.uid, tgt.uid_len);
                s_uid_len    = tgt.uid_len;
                s_card_type  = tgt.type;
                s_magic_done = (tgt.type != PN532_CARD_MIFARE_CLASSIC_1K &&
                                tgt.type != PN532_CARD_MIFARE_CLASSIC_4K);
                state        = STATE_PRESENT;
                fail_count   = 0;
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }

        } else {
            // PRESENT：首次进入若是 Classic 卡，先补一次 magic 检测；
            // 之后改为短超时 ping 卡是否还在
            if (!s_magic_done) {
                mfc_magic_type_t magic = mfc_detect_magic();
                nfc_release();
                s_magic_done           = true;
                const char* magic_name = mfc_magic_str(magic);
                if (magic_name) {
                    ws_card_meta(magic_name);
                    ESP_LOGI(TAG, "card magic=%s", magic_name);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            } else {
                pn532_target_t tgt;
                esp_err_t      r = pn532_read_passive_target(&tgt, 150);
                nfc_release();
                if (r == ESP_OK) {
                    fail_count = 0;
                    vTaskDelay(pdMS_TO_TICKS(300));
                } else {
                    fail_count++;
                    if (fail_count >= 2) {
                        ws_card_out();
                        ESP_LOGI(TAG, "card out");
                        state      = STATE_SCANNING;
                        fail_count = 0;
                        s_uid_len  = 0;
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
            }
        }

        // 短暂 yield，让 httpd 高优先级任务有机会抢锁
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t nfc_monitor_start(void) {
    if (xTaskCreate(monitor_task, "nfc_mon", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "nfc monitor started");
    return ESP_OK;
}

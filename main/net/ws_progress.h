#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_http_server.h"

void ws_progress_init(httpd_handle_t srv);

esp_err_t ws_progress_handler(httpd_req_t* req);

void ws_progress_broadcast(const char* json);

void ws_progress_sector(const char* task_id, uint8_t sector, uint8_t total,
                        bool key_a_found, bool key_b_found);

void ws_progress_done(const char* task_id, const char* result_json);
void ws_progress_error(const char* task_id, const char* msg);

void ws_card_in(const char* uid_hex, uint8_t card_type, const char* type_name, const char* magic);
void ws_card_meta(const char* magic);
void ws_card_out(void);

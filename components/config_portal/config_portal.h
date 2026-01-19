#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

// Initialize the settings module (call once at startup)
void config_portal_init(void);

httpd_handle_t config_portal_start(void);
void config_portal_stop(httpd_handle_t server);

// Save/load single key-value pair
esp_err_t save_setting(const char* key, const char* value);
esp_err_t load_setting(const char* key, char* value, size_t max_len);

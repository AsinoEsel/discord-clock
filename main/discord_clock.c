#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "wifi_portal";

// ======= CONFIG =======
#define AP_SSID "ESP32-Portal"
#define AP_MAX_CONN 4

// ======= FUNCTION DECLARATIONS =======
void start_ap(void);
void start_sta(const char* ssid, const char* pass);
void wifi_event_handler(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data);
httpd_handle_t start_webserver(void);
void stop_webserver(httpd_handle_t server);
void save_credentials(const char* ssid, const char* pass);
bool load_credentials(char* ssid, size_t ssid_len, char* pass, size_t pass_len);
void connection_success_callback(void);

// ======= HTTP HANDLERS =======
esp_err_t index_get_handler(httpd_req_t *req) {
    const char* resp =
        "<!DOCTYPE html><html><body>"
        "<h1>ESP32 Captive Portal</h1>"
        "<form action='/save' method='post'>"
        "SSID: <input name='ssid'><br>"
        "Password: <input name='pass'><br>"
        "<input type='submit'></form></body></html>";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = 0;

    // Very simple parser (assuming ssid=...&pass=...)
    char ssid[32] = {0};
    char pass[64] = {0};
    sscanf(buf, "ssid=%31[^&]&pass=%63s", ssid, pass);

    ESP_LOGI(TAG, "Received SSID='%s', PASS='%s'", ssid, pass);
    save_credentials(ssid, pass);

    httpd_resp_sendstr(req, "Credentials saved. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000)); // wait 1s so browser sees response

    esp_restart(); // reboot ESP to attempt STA connection
    return ESP_OK;
}

// ======= WEBSERVER =======
httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_get_handler
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t save_uri = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_post_handler
        };
        httpd_register_uri_handler(server, &save_uri);
    }
    return server;
}

void stop_webserver(httpd_handle_t server) {
    if (server) {
        httpd_stop(server);
    }
}

// ======= NVS =======
void save_credentials(const char* ssid, const char* pass) {
    nvs_handle_t handle;
    nvs_open("wifi", NVS_READWRITE, &handle);
    nvs_set_str(handle, "ssid", ssid);
    nvs_set_str(handle, "pass", pass);
    nvs_commit(handle);
    nvs_close(handle);
}

bool load_credentials(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READONLY, &handle) != ESP_OK) return false;
    if (nvs_get_str(handle, "ssid", ssid, &ssid_len) != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    if (nvs_get_str(handle, "pass", pass, &pass_len) != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    nvs_close(handle);
    return true;
}

// ======= WIFI STA/AP LOGIC =======
static bool sta_connected = false;
static httpd_handle_t server = NULL;

void wifi_event_handler(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "STA disconnected, fallback to AP");
                sta_connected = false;
                start_ap();
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Device connected to AP");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Device disconnected from AP");
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    esp_ip4_addr_t ip = event->ip_info.ip;  // ip4_addr_t is uint32_t
    ESP_LOGI(TAG, "Connected! IP=" IPSTR, IP2STR(&ip));
    sta_connected = true;
    connection_success_callback();
    }
}

void start_ap(void) {
    if (server) stop_webserver(server);
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = 0,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN
        }
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "AP started: SSID=%s", AP_SSID);
    server = start_webserver();
}

void start_sta(const char* ssid, const char* pass) {
    if (server) stop_webserver(server); // stop portal
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t sta_config = {0};
    strncpy((char*)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char*)sta_config.sta.password, pass, sizeof(sta_config.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_start();
}

// ======= SUCCESS CALLBACK =======
void connection_success_callback(void) {
    ESP_LOGI(TAG, "STA connected successfully! Callback triggered.");
    // Could add OTA / time sync / MQTT connect here
}

// ======= MAIN =======
void app_main(void) {
    ESP_LOGI(TAG, "Starting Wi-Fi captive portal example");

    // 1. Init NVS, TCP/IP stack, event loop
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &instance_any_id);

    char ssid[32], pass[64];
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "Found saved credentials, trying STA...");
        start_sta(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No saved credentials, starting AP...");
        start_ap();
    }
}

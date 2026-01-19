#include "config_portal.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "ctype.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"

static const char* TAG = "config_portal";


// ======= Forward declarations =======
static esp_err_t index_get_handler(httpd_req_t *req);
static esp_err_t css_get_handler(httpd_req_t *req);
static esp_err_t save_post_handler(httpd_req_t *req);


// Public API
void config_portal_init(void);
httpd_handle_t config_portal_start(void);
void config_portal_stop(httpd_handle_t server);


// Defaults
#define DEFAULT_COLOR "#23A55A"


// Helper stuff

static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a = a - 'A' + 10; else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b = b - 'A' + 10; else b -= '0';
            *dst++ = 16*a + b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}


// ======= Initialization =======

void config_portal_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_LOGI(TAG, "Config portal module initialized");
}

// ================= NVS =================
esp_err_t save_setting(const char* key, const char* value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved key='%s', value='%s'", key, value);
    return err;
}

esp_err_t load_setting(const char* key, char* value, size_t max_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, key, value, &max_len);
    nvs_close(handle);
    return err;
}


// ======= Embedded assets =======
extern const unsigned char _binary_index_html_start[];
extern const unsigned char _binary_index_html_end[];
extern const unsigned char _binary_style_css_start[];
extern const unsigned char _binary_style_css_end[];

// ======= HTTP handlers =======
static esp_err_t index_get_handler(httpd_req_t *req) {
    char led_color[8] = DEFAULT_COLOR;

    // Try to load from NVS
    load_setting("led_color", led_color, sizeof(led_color));

    // Generate HTML dynamically from template
    char buf[1024]; // big enough for simple template
    const char *template = (const char *)_binary_index_html_start;

    // Replace {{LED_COLOR}} placeholder
    const char *placeholder = "{{LED_COLOR}}";
    char *pos = strstr(template, placeholder);
    if (pos) {
        size_t placeholder_len = strlen(placeholder);
        size_t prefix_len = pos - template;
        size_t suffix_len = strlen(pos + placeholder_len);

        // Build final HTML into buf
        memcpy(buf, template, prefix_len);
        memcpy(buf + prefix_len, led_color, strlen(led_color));
        memcpy(buf + prefix_len + strlen(led_color), pos + placeholder_len, suffix_len + 1); // +1 for null
    } else {
        strncpy(buf, template, sizeof(buf));
        buf[sizeof(buf)-1] = '\0';
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, strlen(buf));
    return ESP_OK;
}


static esp_err_t css_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(
        req,
        (const char *)_binary_style_css_start,
        _binary_style_css_end - _binary_style_css_start
    );
    return ESP_OK;
}


// ================= HTTP Handler =================
esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[512]; // buffer for form data
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = 0; // null terminate

    bool reboot_needed = false;

    char *pair = strtok(buf, "&");
    while (pair) {
        char key[32], value[128];
        if (sscanf(pair, "%31[^=]=%127s", key, value) == 2) {
            char decoded[128];
            url_decode(decoded, value);          // <--- decode URL encoding
            save_setting(key, decoded);

            if (strcmp(key, "ssid") == 0 || strcmp(key, "pass") == 0) {
                reboot_needed = true;
            }
        }
        pair = strtok(NULL, "&");
    }

        if (reboot_needed) {
        // For Wi-Fi changes, reboot anyway
        httpd_resp_sendstr(req, "Wi-Fi settings saved. Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        // Redirect to main page instead of staying on /save
        httpd_resp_set_status(req, "303 See Other");   // 303 is preferred after POST
        httpd_resp_set_hdr(req, "Location", "/?saved=1");     // redirect to index.html
        httpd_resp_sendstr(req, "Redirecting...");
    }

    return ESP_OK;
}



static esp_err_t settings_get_handler(httpd_req_t *req) {
    char ssid[32] = "";
    char pass[64] = "";
    char led_color[8] = DEFAULT_COLOR;

    load_setting("ssid", ssid, sizeof(ssid));
    load_setting("pass", pass, sizeof(pass));
    load_setting("led_color", led_color, sizeof(led_color));

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "{ \"ssid\": \"%s\", \"pass\": \"%s\", \"led_color\": \"%s\" }",
        ssid, pass, led_color);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}


httpd_handle_t config_portal_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_get_handler
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t css_uri = {
            .uri = "/style.css",
            .method = HTTP_GET,
            .handler = css_get_handler
        };
        httpd_register_uri_handler(server, &css_uri);

        httpd_uri_t save_uri = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_post_handler
        };
        httpd_register_uri_handler(server, &save_uri);

        // Register JSON API for current settings
        httpd_uri_t settings_uri = {
            .uri = "/settings.json",
            .method = HTTP_GET,
            .handler = settings_get_handler
        };
        httpd_register_uri_handler(server, &settings_uri);
    }
    return server;
}


void config_portal_stop(httpd_handle_t server) {
    if (server) {
        httpd_stop(server);
    }
}

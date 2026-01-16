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

#include "driver/gpio.h"
#include "discord.h"
#include "discord/session.h"
#include "discord/voice_state.h"

#include "led_strip.h"

#define LED_STRIP_GPIO_PIN  16
#define LED_STRIP_LED_COUNT 24
#define LED_STRIP_RMT_RES_HZ  (10 * 1000 * 1000)  // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)

static led_strip_handle_t led_strip = NULL; // Global handle for the LED strip
static discord_handle_t bot;


static const char *TAG = "wifi_portal";

// ======= CONFIG =======
#define AP_MAX_CONN 4  // Maximum number of connections to the AP
static char saved_color[8] = "#23A55A"; // Default color


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


/* LED CODE */

led_strip_handle_t configure_led(void)
{
    // LED strip general initialization, according to your led board design
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO_PIN, // The GPIO that connected to the LED strip's data line
        .max_leds = LED_STRIP_LED_COUNT,      // The number of LEDs in the strip,
        .led_model = LED_MODEL_WS2812,        // LED strip model
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // The color order of the strip: GRB
        .flags = {
            .invert_out = false, // don't invert the output signal
        }
    };

    // LED strip backend configuration: RMT
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
        .resolution_hz = LED_STRIP_RMT_RES_HZ, // RMT counter clock frequency
        .mem_block_symbols = 64,               // the memory size of each RMT channel, in words (4 bytes)
        .flags = {
            .with_dma = false, // DMA feature is available on chips like ESP32-S3/P4
        }
    };

    // LED Strip object handle
    led_strip_handle_t led_strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_LOGI(TAG, "Created LED strip object with RMT backend");
    return led_strip;
}


static void parse_color(const char *color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (color[0] == '#' && strlen(color) == 7) {
        // Parse the red, green, and blue components
        char red[3] = {color[1], color[2], '\0'};   // First two hex digits after #
        char green[3] = {color[3], color[4], '\0'}; // Next two hex digits
        char blue[3] = {color[5], color[6], '\0'};  // Last two hex digits

        *r = (uint8_t)strtol(red, NULL, 16);
        *g = (uint8_t)strtol(green, NULL, 16);
        *b = (uint8_t)strtol(blue, NULL, 16);
    } else {
        // Default to black if the format is incorrect
        *r = 100;
        *g = 0;
        *b = 0;
    }
}


void set_leds(bool on)
{
    uint8_t red, green, blue;
    parse_color(saved_color, &red, &green, &blue);
    
    if (led_strip == NULL) {
        ESP_LOGE(TAG, "LED strip not initialized");
        return;
    }

    if (!on) {
        // Turn off all LEDs
        ESP_ERROR_CHECK(led_strip_clear(led_strip));
        ESP_LOGI(TAG, "LEDs turned OFF");
    } else {
        ESP_LOGI(TAG, "Setting LEDs to color: R=%d, G=%d, B=%d", red, green, blue);
        // Turn on all LEDs with a dim white color
        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, i, red, green, blue));
        }
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    }
}


/* DISCORD CODE */

/**
 * GPIO number of the LED that will be ON when the user is muted.
 * Otherwise, when the user is not muted, LED will be OFF.
 */
const gpio_num_t LED_GPIO = GPIO_NUM_2;

typedef struct user_voice_state {
    char user_id[64];    // Store user ID
    char channel_id[64]; // Store channel ID (if in a voice channel)
    struct user_voice_state* next;
} user_voice_state_t;

static user_voice_state_t* voice_states_head = NULL; // Linked list head for tracking voice states
static int voice_channel_user_count = 0;            // Global counter for users in voice chat

// Add or update a user's voice state
void update_user_voice_state(const char* user_id, const char* channel_id) {
    user_voice_state_t* current = voice_states_head;
    user_voice_state_t* prev = NULL;

    // Search for the user in the linked list
    while (current != NULL) {
        if (strcmp(current->user_id, user_id) == 0) {
            // User found
            if (channel_id == NULL && current->channel_id[0] != '\0') {
                // User left a voice channel
                voice_channel_user_count--;
                ESP_LOGI(TAG, "User %s left voice chat. Count: %d", user_id, voice_channel_user_count);
            } else if (channel_id != NULL && current->channel_id[0] == '\0') {
                // User joined a voice channel
                voice_channel_user_count++;
                ESP_LOGI(TAG, "User %s joined voice chat. Count: %d", user_id, voice_channel_user_count);
            }

            // Update the user's channel ID
            if (channel_id) {
                strncpy(current->channel_id, channel_id, sizeof(current->channel_id) - 1);
                current->channel_id[sizeof(current->channel_id) - 1] = '\0'; // Null-terminate
            } else {
                current->channel_id[0] = '\0'; // Clear channel ID
            }
            return;
        }
        prev = current;
        current = current->next;
    }

    // User not found: Add new user to the list
    user_voice_state_t* new_user = (user_voice_state_t*)malloc(sizeof(user_voice_state_t));
    if (!new_user) {
        ESP_LOGE(TAG, "Failed to allocate memory for new user");
        return;
    }
    strncpy(new_user->user_id, user_id, sizeof(new_user->user_id) - 1);
    new_user->user_id[sizeof(new_user->user_id) - 1] = '\0'; // Null-terminate
    if (channel_id) {
        strncpy(new_user->channel_id, channel_id, sizeof(new_user->channel_id) - 1);
        new_user->channel_id[sizeof(new_user->channel_id) - 1] = '\0'; // Null-terminate
        voice_channel_user_count++;
        ESP_LOGI(TAG, "User %s joined voice chat for the first time. Count: %d", user_id, voice_channel_user_count);
    } else {
        new_user->channel_id[0] = '\0'; // No channel
    }
    new_user->next = NULL;

    // Append to the linked list
    if (prev) {
        prev->next = new_user;
    } else {
        voice_states_head = new_user;
    }
}

// Event handler
static void bot_event_handler(void* handler_arg, esp_event_base_t base, int32_t event_id, void* event_data) {
    discord_event_data_t* data = (discord_event_data_t*)event_data;

    switch (event_id) {
        case DISCORD_EVENT_CONNECTED: {
            discord_session_t* session = (discord_session_t*)data->ptr;
            ESP_LOGI(TAG, "Bot %s#%s connected",
                     session->user->username,
                     session->user->discriminator);
        } break;

        case DISCORD_EVENT_VOICE_STATE_UPDATED: {
            discord_voice_state_t* vstate = (discord_voice_state_t*)data->ptr;

            ESP_LOGI(TAG, "voice_state (user_id=%s, channel_id=%s, mute=%d, self_mute=%d, deaf=%d, self_deaf=%d)",
                     vstate->user_id,
                     vstate->channel_id ? vstate->channel_id : "NULL",
                     vstate->mute,
                     vstate->self_mute,
                     vstate->deaf,
                     vstate->self_deaf);

            // Update the user's voice state
            update_user_voice_state(vstate->user_id, vstate->channel_id);

            // Example action: Toggle LED based on user count
            if (voice_channel_user_count > 0) {
                set_leds(true);
                gpio_set_level(LED_GPIO, 1);  // Turn LED on
            } else {
                set_leds(false);
                gpio_set_level(LED_GPIO, 0);  // Turn LED off
            }
        } break;

        case DISCORD_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Bot logged out");
            break;

        default:
            break;
    }
}

// Free linked list memory
void free_voice_states() {
    user_voice_state_t* current = voice_states_head;
    while (current) {
        user_voice_state_t* next = current->next;
        free(current);
        current = next;
    }
}


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

    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;

    if (nvs_get_str(handle, "ssid", ssid, &ssid_size) != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    if (nvs_get_str(handle, "pass", pass, &pass_size) != ESP_OK) {
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
    static int sta_retry_count = 0;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                if (sta_retry_count < CONFIG_MAX_STA_RETRIES) {
                    ESP_LOGI(TAG, "STA disconnected, retrying...");
                    esp_wifi_connect();
                    sta_retry_count++;
                } else {
                    ESP_LOGI(TAG, "STA failed, fallback to AP");
                    sta_connected = false;
                    start_ap();
                }
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Device connected to AP");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Device disconnected from AP");
                break;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;

        ESP_LOGI(TAG, "STA connected! IP=" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask=" IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway=" IPSTR, IP2STR(&event->ip_info.gw));

        sta_connected = true;
        connection_success_callback();
    }
}


void start_ap(void) {
    if (server) stop_webserver(server);

    wifi_config_t ap_config = {0};
    strncpy((char*)ap_config.ap.ssid, CONFIG_AP_SSID, sizeof(ap_config.ap.ssid));
    strncpy((char*)ap_config.ap.password, CONFIG_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.max_connection = AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID='%s'", CONFIG_AP_SSID);
    server = start_webserver();
}


void start_sta(const char* ssid, const char* pass) {
    wifi_config_t sta_config = {0};
    strncpy((char*)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char*)sta_config.sta.password, pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA started. Trying to connect to SSID='%s'", ssid);
}


// ======= SUCCESS CALLBACK =======
void connection_success_callback(void) {
    ESP_LOGI(TAG, "STA connected successfully! Callback triggered.");

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    led_strip = configure_led();

    discord_config_t cfg = {
       .intents = DISCORD_INTENT_GUILD_VOICE_STATES
    };

    bot = discord_create(&cfg);
    ESP_ERROR_CHECK(discord_register_events(bot, DISCORD_EVENT_ANY, bot_event_handler, NULL));
    ESP_ERROR_CHECK(discord_login(bot));
}


// ======= MAIN =======
void app_main(void) {
    ESP_LOGI(TAG, "Starting Wi-Fi captive portal example");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default interfaces
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // Initialize Wi-Fi once
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    // Register event handlers
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &instance_any_id);

    // Decide STA vs AP
    char ssid[32], pass[64];
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "Found saved credentials, starting STA...");
        start_sta(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No saved credentials, starting AP...");
        start_ap();
    }
}

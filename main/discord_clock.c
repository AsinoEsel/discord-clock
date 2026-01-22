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
#include "mdns.h"

#include "driver/gpio.h"
#include "discord.h"
#include "discord/session.h"
#include "discord/voice_state.h"

#include "led_strip.h"

#include "config_portal.h"

#include "led_animation.h"


static discord_handle_t bot;
static const char *TAG = "discord_clock";


// ======= FUNCTION DECLARATIONS =======
void start_ap(void);
void start_sta(const char* ssid, const char* pass);
void wifi_event_handler(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data);
void connection_success_callback(void);


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
                led_animation_set(LED_ANIM_SOLID);
                gpio_set_level(LED_GPIO, 1);  // Turn LED on
            } else {
                led_animation_set(LED_ANIM_OFF);
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
    if (server) config_portal_stop(server);

    wifi_config_t ap_config = {0};
    strncpy((char*)ap_config.ap.ssid, CONFIG_AP_SSID, sizeof(ap_config.ap.ssid));
    strncpy((char*)ap_config.ap.password, CONFIG_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.max_connection = CONFIG_AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID='%s'", CONFIG_AP_SSID);
    server = config_portal_start();
}


void start_sta(const char* ssid, const char* pass) {
    wifi_config_t sta_config = {0};
    strncpy((char*)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char*)sta_config.sta.password, pass, sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Set DHCP hostname
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, CONFIG_DEVICE_NAME));

    ESP_LOGI(TAG, "STA started. Trying to connect to SSID='%s'", ssid);
}


static void init_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_DEVICE_NAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("Discord Clock"));

    // Advertise HTTP service
    ESP_ERROR_CHECK(mdns_service_add(
        "Discord Clock Settings Portal",
        "_http",
        "_tcp",
        80,
        NULL,
        0
    ));
}


// ======= SUCCESS CALLBACK =======
void connection_success_callback(void) {
    ESP_LOGI(TAG, "STA connected successfully! Callback triggered.");

    init_mdns();

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    if (!server) {
        server = config_portal_start();
    }

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

    // Init settings
    config_portal_init();

    // Initialize Wi-Fi once
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    // Initialize LED Strip

    led_strip_handle_t strip;

    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_LED_STRIP_GPIO,
        .max_leds = CONFIG_LED_STRIP_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(
        led_strip_new_rmt_device(&strip_config, &rmt_config, &strip)
    );

    led_animation_init(strip);
    led_animation_set(LED_ANIM_SOLID);

    // Register event handlers
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &instance_any_id);

    // Decide STA vs AP
    char ssid[32] = {0};
    char pass[64] = {0};

    bool have_ssid =
        load_setting("ssid", ssid, sizeof(ssid)) == ESP_OK;
    bool have_pass =
        load_setting("pass", pass, sizeof(pass)) == ESP_OK;

    if (have_ssid && have_pass) {
        ESP_LOGI(TAG, "Found saved credentials, starting STA...");
        start_sta(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No saved credentials, starting AP...");
        start_ap();
    }

}

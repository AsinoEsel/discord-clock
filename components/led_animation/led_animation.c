#include "led_animation.h"
#include "config_portal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define DEFAULT_COLOR "#800000"  // fallback color if NVS not found
#define LED_TASK_STACK 2048
#define LED_TASK_PRIORITY 5
#define LED_UPDATE_MS 50

static led_strip_handle_t strip_handle = NULL;
static led_animation_type_t current_animation = LED_ANIM_OFF;
static uint8_t led_color[3];

static void led_task(void *arg);
static void parse_hex_color(const char *hex, uint8_t *rgb);

void led_animation_init(led_strip_handle_t strip)
{
    strip_handle = strip;

    char color_str[8] = {0};
    if (load_setting("led_color", color_str, sizeof(color_str)) != ESP_OK) {
        strncpy(color_str, DEFAULT_COLOR, sizeof(color_str));
    }
    parse_hex_color(color_str, led_color);

    xTaskCreate(led_task, "led_animation", LED_TASK_STACK, NULL,
                LED_TASK_PRIORITY, NULL);
}

void led_animation_set(led_animation_type_t anim)
{
    current_animation = anim;
}

// ================== Internal helpers ==================

static void parse_hex_color(const char *color, uint8_t *rgb)
{
    if (color[0] == '#' && strlen(color) == 7) {
        // Parse the red, green, and blue components
        char red[3] = {color[1], color[2], '\0'};   // First two hex digits after #
        char green[3] = {color[3], color[4], '\0'}; // Next two hex digits
        char blue[3] = {color[5], color[6], '\0'};  // Last two hex digits

        rgb[0] = (uint8_t)strtol(red, NULL, 16);
        rgb[1] = (uint8_t)strtol(green, NULL, 16);
        rgb[2] = (uint8_t)strtol(blue, NULL, 16);
    } else {
        // Default to red if the format is incorrect
        rgb[0] = 100;
        rgb[1] = 0;
        rgb[2] = 0;
    }
}

static void led_task(void *arg)
{
    int blink = 0;

    while (1) {
        switch (current_animation) {
        case LED_ANIM_OFF:
            led_strip_clear(strip_handle);
            break;

        case LED_ANIM_SOLID:
            for (int i = 0; i < CONFIG_LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(strip_handle, i,
                                    led_color[0], led_color[1], led_color[2]);
            }
            led_strip_refresh(strip_handle);
            break;

        case LED_ANIM_BLINK:
            blink = !blink;
            for (int i = 0; i < CONFIG_LED_STRIP_LED_COUNT; i++) {
                led_strip_set_pixel(strip_handle, i,
                                    blink ? led_color[0] : 0,
                                    blink ? led_color[1] : 0,
                                    blink ? led_color[2] : 0);
            }
            led_strip_refresh(strip_handle);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(LED_UPDATE_MS));
    }
}
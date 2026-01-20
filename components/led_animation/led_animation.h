#pragma once

#include "led_strip.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_ANIM_OFF = 0,
    LED_ANIM_SOLID,
    LED_ANIM_BLINK,
} led_animation_type_t;

/**
 * @brief Initialize LED animation system
 */
void led_animation_init(led_strip_handle_t strip);

/**
 * @brief Change current animation
 */
void led_animation_set(led_animation_type_t anim);

#ifdef __cplusplus
}
#endif

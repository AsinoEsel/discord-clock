idf_component_register(
    SRCS "discord_clock.c"
    INCLUDE_DIRS "."
    REQUIRES led_strip esp-discord config_portal led_animation
    PRIV_REQUIRES esp_netif esp_wifi esp_http_server nvs_flash driver
)

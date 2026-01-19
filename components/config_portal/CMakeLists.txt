idf_component_register(
    SRCS "config_portal.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server nvs_flash
    EMBED_FILES "web/index.html" "web/style.css"
)

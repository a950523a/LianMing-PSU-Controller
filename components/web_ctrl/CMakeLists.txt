idf_component_register(
    SRCS "src/web_ctrl.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        core_logic
        freertos
    PRIV_REQUIRES
        esp_http_server
        app_update
        esp_wifi
        esp_system
        json
        nvs_flash
        log
)

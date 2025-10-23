idf_component_register(
    SRCS 
        "main.c"
    INCLUDE_DIRS 
        "."
    REQUIRES 
        nvs_flash 
        app_update
        esp_https_ota 
        esp_http_client
        esp_wifi
        esp_event
        lwip
        freertos
        driver  # ADD THIS LINE
)

idf_component_register(
    SRCS 
        "main.c"
    INCLUDE_DIRS 
        "."
    REQUIRES 
        nvs_flash 
        esp_https_ota 
        esp_http_client
        esp_wifi
        esp_event
        driver
        app_update
        esp_partition
        efuse
        json  # Added for robust JSON parsing (cJSON)
)

set(COMPONENT_VER "1.0.0")

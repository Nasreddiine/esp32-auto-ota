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
        bootloader_support
        efuse
        mbedtls
        esp_crt_bundle
)

set(COMPONENT_VER "1.0.0")

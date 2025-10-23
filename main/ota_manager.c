#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "cJSON.h"

static const char *TAG = "OTA_MANAGER";

// Configuration - UPDATE WITH YOUR REPOSITORY
#define VERSION_URL "https://raw.githubusercontent.com/Nasreddiine/esp32-auto-ota/main/version.json"
#define FIRMWARE_URL "https://github.com/Nasreddiine/esp32-auto-ota/releases/latest/download/firmware.bin"

// Current version - will be compared with online version
#define CURRENT_VERSION "initial"

static char latest_version[50] = "";
static char download_url[200] = "";
static bool update_available = false;

// HTTP event handler
esp_err_t http_event_handler(esp_http_client_event_t *e) {
    switch(e->event_id) {
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", e->data_len);
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Check for updates by fetching version.json
void check_for_updates(void) {
    ESP_LOGI(TAG, "🔍 Checking for updates...");
    
    esp_http_client_config_t config = {
        .url = VERSION_URL,
        .event_handler = http_event_handler,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client");
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "📡 HTTP Status: %d", status_code);
        
        if (status_code == 200) {
            int content_length = esp_http_client_get_content_length(client);
            if (content_length > 0) {
                char *response = malloc(content_length + 1);
                int read_len = esp_http_client_read(client, response, content_length);
                response[read_len] = '\0';
                
                ESP_LOGI(TAG, " Version info: %s", response);
                
                // Parse JSON response
                cJSON *root = cJSON_Parse(response);
                if (root != NULL) {
                    cJSON *version = cJSON_GetObjectItem(root, "version");
                    cJSON *url = cJSON_GetObjectItem(root, "url");
                    
                    if (cJSON_IsString(version) && cJSON_IsString(url)) {
                        strncpy(latest_version, version->valuestring, sizeof(latest_version) - 1);
                        strncpy(download_url, url->valuestring, sizeof(download_url) - 1);
                        
                        ESP_LOGI(TAG, " Current: %s, Latest: %s", CURRENT_VERSION, latest_version);
                        
                        if (strcmp(CURRENT_VERSION, latest_version) != 0) {
                            update_available = true;
                            ESP_LOGI(TAG, " Update available! New version: %s", latest_version);
                        } else {
                            ESP_LOGI(TAG, " Firmware is up to date");
                        }
                    }
                    cJSON_Delete(root);
                }
                free(response);
            }
        }
    } else {
        ESP_LOGE(TAG, " HTTP request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
}

// Perform OTA update
void perform_ota_update(void) {
    if (!update_available || strlen(download_url) == 0) {
        ESP_LOGE(TAG, " No update available or no download URL");
        return;
    }
    
    ESP_LOGI(TAG, "🚀 Starting OTA update from: %s", download_url);
    
    esp_http_client_config_t config = {
        .url = download_url,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, " OTA update successful! Restarting...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, " OTA update failed: %s", esp_err_to_name(ret));
    }
}

// Public functions
void ota_init(void) {
    ESP_LOGI(TAG, "🔄 OTA Manager initialized");
    strcpy(latest_version, CURRENT_VERSION);
}

void ota_check_update(void) {
    check_for_updates();
}

bool ota_is_update_available(void) {
    return update_available;
}

void ota_perform_update(void) {
    perform_ota_update();
}

const char* ota_get_current_version(void) {
    return CURRENT_VERSION;
}
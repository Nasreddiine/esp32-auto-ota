#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "cJSON.h"

// WiFi Configuration
#define WIFI_SSID "INPT-Residence"
#define WIFI_PASS "iinnpptt"

// LED Configuration
#define BLINK_GPIO GPIO_NUM_2

// GitHub OTA Configuration
#define GITHUB_USER "Nasreddiine"
#define GITHUB_REPO "esp32-auto-ota"
#define FIRMWARE_VERSION "1.0.0"

// GitHub URLs
#define VERSION_JSON_URL "https://raw.githubusercontent.com/" GITHUB_USER "/" GITHUB_REPO "/main/version.json"
#define FIRMWARE_BIN_URL "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/firmware.bin"

static const char *TAG = "OTA_APP";

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi started");
}

bool check_for_updates(void) {
    ESP_LOGI(TAG, "Checking GitHub for updates...");
    
    esp_http_client_config_t config = {
        .url = VERSION_JSON_URL,
        .timeout_ms = 15000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }
    
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to GitHub: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    
    char buffer[512];
    int content_length = esp_http_client_fetch_headers(client);
    int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
    
    bool update_available = false;
    
    if (read_len > 0) {
        buffer[read_len] = '\0';
        ESP_LOGI(TAG, "Received version info");
        
        cJSON *json = cJSON_Parse(buffer);
        if (json != NULL) {
            cJSON *version = cJSON_GetObjectItem(json, "version");
            if (cJSON_IsString(version) && version->valuestring != NULL) {
                ESP_LOGI(TAG, "Current: %s, Available: %s", FIRMWARE_VERSION, version->valuestring);
                
                if (strcmp(version->valuestring, FIRMWARE_VERSION) != 0) {
                    ESP_LOGI(TAG, "New version available: %s", version->valuestring);
                    update_available = true;
                } else {
                    ESP_LOGI(TAG, "Already running latest version");
                }
            } else {
                ESP_LOGE(TAG, "Invalid version format in JSON");
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGE(TAG, "Failed to parse JSON");
        }
    } else {
        ESP_LOGE(TAG, "Failed to read version info");
    }
    
    esp_http_client_cleanup(client);
    return update_available;
}

void perform_ota_update(void) {
    ESP_LOGI(TAG, "Starting OTA update from GitHub...");
    
    esp_http_client_config_t config = {
        .url = FIRMWARE_BIN_URL,
        .timeout_ms = 60000,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    
    ESP_LOGI(TAG, "Downloading firmware from: %s", FIRMWARE_BIN_URL);
    
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Update Successful! Rebooting...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Update Failed: %s", esp_err_to_name(ret));
    }
}

void blink_led_pattern(int times, int delay_ms) {
    for (int i = 0; i < times; i++) {
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(delay_ms / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(delay_ms / portTICK_PERIOD_MS);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32 GitHub Auto-OTA ===");
    ESP_LOGI(TAG, "Version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "Repo: %s/%s", GITHUB_USER, GITHUB_REPO);
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Setup LED
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    ESP_LOGI(TAG, "LED initialized on GPIO %d", BLINK_GPIO);
    
    // Connect to WiFi
    wifi_init();
    ESP_LOGI(TAG, "Connecting to WiFi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected!");
    
    // Check for updates immediately after boot
    ESP_LOGI(TAG, "Checking for updates...");
    if (check_for_updates()) {
        ESP_LOGI(TAG, "Update found! Starting download...");
        blink_led_pattern(8, 150);
        perform_ota_update();
    }
    
    ESP_LOGI(TAG, "Running main application");
    
    // Main application loop
    int counter = 0;
    while (1) {
        // Normal operation - slow blink
        gpio_set_level(BLINK_GPIO, 1);
        ESP_LOGI(TAG, "Running - Cycle: %d", counter);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        
        counter++;
        
        // Check for updates every 10 minutes
        if (counter % 600 == 0) {
            ESP_LOGI(TAG, "Scheduled update check...");
            if (check_for_updates()) {
                ESP_LOGI(TAG, "Update available! Starting OTA...");
                blink_led_pattern(10, 100);
                perform_ota_update();
            }
        }
        
        // Show status every 10 cycles
        if (counter % 10 == 0) {
            ESP_LOGI(TAG, "Still running - Total cycles: %d", counter);
        }
    }
}

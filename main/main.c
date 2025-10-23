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

// WiFi Configuration
#define WIFI_SSID "INPT-Residence"
#define WIFI_PASS "iinnpptt"

// LED Configuration
#define BLINK_GPIO GPIO_NUM_2

// GitHub OTA Configuration
#define GITHUB_USER "Nasreddiine"
#define GITHUB_REPO "esp32-auto-ota"
#define FIRMWARE_VERSION "1.0.0"

// GitHub URLs - Use direct firmware URL for simplicity
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

// Check if we should update by comparing with current running firmware
bool should_update(void) {
    ESP_LOGI(TAG, "Checking if update needed...");
    
    // Get current running firmware info
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Currently running: %s", running_app->version);
    ESP_LOGI(TAG, "This firmware: %s", FIRMWARE_VERSION);
    
    // Simple approach: Always try to update if we have a new version in code
    // This ensures the ESP32 will update to whatever version is in FIRMWARE_VERSION
    if (strcmp(running_app->version, FIRMWARE_VERSION) != 0) {
        ESP_LOGI(TAG, "Update needed: running %s, want %s", running_app->version, FIRMWARE_VERSION);
        return true;
    }
    
    ESP_LOGI(TAG, "No update needed - already running version %s", FIRMWARE_VERSION);
    return false;
}

void perform_ota_update(void) {
    ESP_LOGI(TAG, "Starting OTA update from GitHub...");
    ESP_LOGI(TAG, "Download URL: %s", FIRMWARE_BIN_URL);
    
    esp_http_client_config_t config = {
        .url = FIRMWARE_BIN_URL,
        .timeout_ms = 90000, // 90 seconds for larger firmware
        .buffer_size_tx = 4096,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .bulk_flash_erase = true, // Faster for large updates
    };
    
    ESP_LOGI(TAG, "Initializing OTA...");
    esp_err_t ret = esp_https_ota(&ota_config);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Update Successful! Rebooting in 3 seconds...");
        
        // Blink rapidly to indicate success
        for(int i = 0; i < 10; i++) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Update Failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Error details: %d", ret);
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
    
    // Check if we need to update
    if (should_update()) {
        ESP_LOGI(TAG, "Update needed! Starting OTA process...");
        blink_led_pattern(5, 200); // Fast blink = updating
        perform_ota_update();
    } else {
        ESP_LOGI(TAG, "Running latest version %s", FIRMWARE_VERSION);
    }
    
    ESP_LOGI(TAG, "Starting main application");
    
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
        
        // Check for updates every 30 minutes
        if (counter % 1800 == 0) { // 1800 seconds = 30 minutes
            ESP_LOGI(TAG, "Periodic update check...");
            if (should_update()) {
                ESP_LOGI(TAG, "Update available! Starting OTA...");
                blink_led_pattern(8, 150);
                perform_ota_update();
            }
        }
        
        // Show status every 30 cycles (30 seconds)
        if (counter % 30 == 0) {
            ESP_LOGI(TAG, "Status: Running version %s - Total cycles: %d", FIRMWARE_VERSION, counter);
        }
    }
}



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

// SET YOUR WIFI HERE
#define WIFI_SSID "INPT-Residence"
#define WIFI_PASS "iinnpptt"

// Try different GPIOs - start with GPIO2
#define BLINK_GPIO GPIO_NUM_2

// UPDATE WITH YOUR GITHUB INFO
#define GITHUB_USER "your_username"
#define GITHUB_REPO "your_repo_name"
#define FIRMWARE_VERSION "1.0.0"

// GitHub URLs - UPDATE THESE!
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
    
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "WiFi started");
}

// Check for updates from GitHub
bool check_for_updates(void) {
    ESP_LOGI(TAG, "Checking for updates...");
    
    esp_http_client_config_t config = {
        .url = VERSION_JSON_URL,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to GitHub: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    
    char buffer[512];
    int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
    
    if (read_len > 0) {
        buffer[read_len] = '\0';
        ESP_LOGI(TAG, "Received version info");
        
        // Parse JSON
        cJSON *json = cJSON_Parse(buffer);
        if (json != NULL) {
            cJSON *version = cJSON_GetObjectItem(json, "version");
            if (cJSON_IsString(version)) {
                if (strcmp(version->valuestring, FIRMWARE_VERSION) != 0) {
                    ESP_LOGI(TAG, "New version available: %s", version->valuestring);
                    cJSON_Delete(json);
                    esp_http_client_cleanup(client);
                    return true;
                }
            }
            cJSON_Delete(json);
        }
    }
    
    ESP_LOGI(TAG, "No update available");
    esp_http_client_cleanup(client);
    return false;
}

// Perform OTA update
void perform_ota_update(void) {
    ESP_LOGI(TAG, "Starting OTA update...");
    
    esp_http_client_config_t config = {
        .url = FIRMWARE_BIN_URL,
        .timeout_ms = 30000,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful, restarting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(ret));
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
    ESP_LOGI(TAG, "Starting ESP32 Auto-OTA - Version: %s", FIRMWARE_VERSION);
    
    // Initialize NVS
    nvs_flash_init();
    
    // Initialize LED GPIO
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    ESP_LOGI(TAG, "LED ready on GPIO %d", BLINK_GPIO);
    
    // Start WiFi and wait for connection
    wifi_init();
    ESP_LOGI(TAG, "Connecting to WiFi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected!");
    
    // Check for updates immediately
    if (check_for_updates()) {
        ESP_LOGI(TAG, "Update found! Downloading...");
        blink_led_pattern(5, 200); // Fast blink for update
        perform_ota_update();
    }
    
    ESP_LOGI(TAG, "Running main application");
    
    // Main loop - simple LED blinking with clean messages
    int counter = 0;
    while (1) {
        // Blink LED every second
        gpio_set_level(BLINK_GPIO, 1);
        ESP_LOGI(TAG, "LED ON - Count: %d", counter);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        
        gpio_set_level(BLINK_GPIO, 0);
        ESP_LOGI(TAG, "LED OFF - Count: %d", counter);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        
        counter++;
        
        // Check for updates every 10 minutes
        if (counter % 600 == 0) {
            ESP_LOGI(TAG, "Checking for updates...");
            if (check_for_updates()) {
                ESP_LOGI(TAG, "Update found! Installing...");
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

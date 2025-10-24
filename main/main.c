#include <stdio.h>
#include <string.h>
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
#define WIFI_SSID "RESIDENCE-1-Etage123"
#define WIFI_PASS "iinnpptt"

// LED Configuration
#define BLINK_GPIO GPIO_NUM_2

// GitHub Configuration
#define GITHUB_USER "Nasreddiine"
#define GITHUB_REPO "esp32-auto-ota"

// URLs
#define VERSION_URL "https://raw.githubusercontent.com/Nasreddiine/esp32_firmware/refs/heads/main/version.txt"
#define FIRMWARE_URL "https://github.com/Nasreddiine/esp32-auto-ota/releases/latest/download/firmware.bin"

// Update check interval - 2 minutes (120 seconds)
#define UPDATE_CHECK_INTERVAL_SECONDS 120

static const char *TAG = "OTA_APP";

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
const int WIFI_FAIL_BIT = BIT1;

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP");
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to ap SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

// Simple version fetch
char* fetch_latest_version(void) {
    ESP_LOGI(TAG, "Fetching latest version...");
    
    // Simple HTTP config - no certificate verification for now
    esp_http_client_config_t config = {
        .url = VERSION_URL,
        .timeout_ms = 10000,
        .cert_pem = NULL,
        .skip_cert_common_name_check = true,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return NULL;
    }
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed: %d", status_code);
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    char version_buffer[32] = {0};
    int read_len = esp_http_client_read(client, version_buffer, sizeof(version_buffer)-1);
    esp_http_client_cleanup(client);
    
    if (read_len <= 0) {
        ESP_LOGE(TAG, "Failed to read version");
        return NULL;
    }
    
    // Trim whitespace
    version_buffer[read_len] = '\0';
    char *end = version_buffer + strlen(version_buffer) - 1;
    while(end >= version_buffer && (*end == '\n' || *end == '\r' || *end == ' ')) {
        *end = '\0';
        end--;
    }
    
    char *latest_version = malloc(strlen(version_buffer) + 1);
    if (latest_version) {
        strcpy(latest_version, version_buffer);
    }
    
    return latest_version;
}

void perform_ota_update(void) {
    ESP_LOGI(TAG, "Starting OTA update...");
    
    // Simple OTA config - no certificate verification
    esp_http_client_config_t config = {
        .url = FIRMWARE_URL,
        .timeout_ms = 120000,
        .cert_pem = NULL,
        .skip_cert_common_name_check = true,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .bulk_flash_erase = false,
    };
    
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Update Successful! Rebooting...");
        
        // Blink LED rapidly to indicate success
        for(int i = 0; i < 10; i++) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Update Failed: %s", esp_err_to_name(ret));
    }
}

void check_for_firmware_update(void) {
    ESP_LOGI(TAG, "Checking for firmware update...");
    
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Current version: %s", running_app->version);
    
    char *latest_version = fetch_latest_version();
    if (!latest_version) {
        ESP_LOGE(TAG, "Failed to fetch latest version");
        return;
    }
    
    ESP_LOGI(TAG, "Latest version: %s", latest_version);
    
    if (strcmp(running_app->version, latest_version) != 0) {
        ESP_LOGI(TAG, "New firmware available! Starting OTA update...");
        perform_ota_update();
    } else {
        ESP_LOGI(TAG, "Device is up to date.");
    }
    
    free(latest_version);
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32 Auto-OTA Version 1.0.1 ===");
    ESP_LOGI(TAG, "Starting device...");
    
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
    
    // Connect to WiFi
    ESP_LOGI(TAG, "Connecting to WiFi: %s", WIFI_SSID);
    wifi_init_sta();
    
    // Check for updates immediately
    check_for_firmware_update();
    
    ESP_LOGI(TAG, "Starting main loop - Double blink pattern (Version 1.0.1)");
    ESP_LOGI(TAG, "Update check interval: %d seconds", UPDATE_CHECK_INTERVAL_SECONDS);
    
    // Main loop - DOUBLE blink pattern for Version 1.0.1
    int seconds_counter = 0;
    while (1) {
        // Version 1.0.1: DOUBLE blink pattern (clearly different!)
        // Pattern: blink-blink-pause (200ms ON, 200ms OFF, 200ms ON, 2400ms OFF)
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(2400 / portTICK_PERIOD_MS);
        
        seconds_counter++;
        
        // Check for updates every 2 minutes (120 seconds)
        if (seconds_counter >= UPDATE_CHECK_INTERVAL_SECONDS) {
            ESP_LOGI(TAG, "Periodic update check after %d seconds...", seconds_counter);
            check_for_firmware_update();
            seconds_counter = 0; // Reset counter
        }
        
        // Show status every 30 seconds
        if (seconds_counter % 30 == 0) {
            ESP_LOGI(TAG, "Running for %d seconds", seconds_counter);
        }
    }
}

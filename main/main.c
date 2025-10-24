#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
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
#include "esp_sntp.h"

// WiFi Configuration
#define WIFI_SSID "INPT-Residence"
#define WIFI_PASS "iinnpptt"

// LED Configuration
#define BLINK_GPIO GPIO_NUM_2

// GitHub OTA Configuration
#define GITHUB_USER "Nasreddiine"
#define GITHUB_REPO "esp32-auto-ota"

// Update check interval (30 seconds for testing)
#define UPDATE_CHECK_INTERVAL_SECONDS 30

// GitHub URLs
#define GITHUB_API_URL "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest"
#define FIRMWARE_BIN_URL "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/firmware.bin"

static const char *TAG = "OTA_APP";

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
const int WIFI_FAIL_BIT = BIT1;

// Skip certificate verification for now
static const char *github_cert = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    ESP_LOGI(TAG, "Starting WiFi...");
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void sync_time(void) {
    ESP_LOGI(TAG, "Setting time from SNTP");
    
    setenv("TZ", "UTC", 1);
    tzset();
    
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_init();
    
    int retry = 0;
    const int retry_count = 10;
    
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "Time synchronized: %s", asctime(&timeinfo));
    } else {
        ESP_LOGW(TAG, "Time synchronization failed, using fallback");
        struct timeval tv = { .tv_sec = 1704067200 }; // Jan 1, 2024
        settimeofday(&tv, NULL);
    }
}

char* extract_version_from_json(const char* json_response) {
    const char* tag_key = "\"tag_name\":\"";
    char* tag_start = strstr(json_response, tag_key);
    if (!tag_start) return NULL;
    
    tag_start += strlen(tag_key);
    char* tag_end = strchr(tag_start, '"');
    if (!tag_end) return NULL;
    
    size_t version_len = tag_end - tag_start;
    char* version = malloc(version_len + 1);
    if (!version) return NULL;
    
    strncpy(version, tag_start, version_len);
    version[version_len] = '\0';
    return version;
}

char* get_latest_version(void) {
    ESP_LOGI(TAG, "Fetching latest version from GitHub...");
    
    // Skip certificate verification
    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .timeout_ms = 15000,
        .cert_pem = github_cert,
        .skip_cert_common_name_check = true,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return NULL;
    }
    
    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client");
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP Status: %d", status_code);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed: %d", status_code);
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    char response[1024] = {0};
    int read_len = esp_http_client_read(client, response, sizeof(response)-1);
    esp_http_client_cleanup(client);
    
    if (read_len <= 0) {
        ESP_LOGE(TAG, "Failed to read response");
        return NULL;
    }
    
    response[read_len] = '\0';
    ESP_LOGI(TAG, "GitHub Response received");
    
    return extract_version_from_json(response);
}

bool should_update(void) {
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Currently running: %s", running_app->version);
    
    char *latest_version = get_latest_version();
    if (!latest_version) {
        ESP_LOGE(TAG, "Failed to get latest version");
        return false;
    }
    
    bool update_needed = strcmp(running_app->version, latest_version) != 0;
    ESP_LOGI(TAG, "Latest version: %s, Update needed: %s", latest_version, update_needed ? "YES" : "NO");
    
    free(latest_version);
    return update_needed;
}

void perform_ota_update(void) {
    ESP_LOGI(TAG, "Starting OTA update...");
    
    // Skip certificate verification for OTA
    esp_http_client_config_t config = {
        .url = FIRMWARE_BIN_URL,
        .timeout_ms = 60000,
        .cert_pem = github_cert,
        .skip_cert_common_name_check = true,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .bulk_flash_erase = false,
    };
    
    esp_err_t err = esp_https_ota(&ota_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA Update Successful! Rebooting...");
        
        // Success blink pattern
        for(int i = 0; i < 10; i++) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Failed: %s", esp_err_to_name(err));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32 Auto-OTA Version 1.0.0 ===");
    
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
    
    // Connect to WiFi with proper event handling
    wifi_init();
    
    // Sync time
    sync_time();
    
    // Check for updates immediately
    ESP_LOGI(TAG, "Performing initial update check...");
    if (should_update()) {
        ESP_LOGI(TAG, "Update available! Starting OTA...");
        perform_ota_update();
    } else {
        ESP_LOGI(TAG, "No update needed - running latest version");
    }
    
    ESP_LOGI(TAG, "Starting main application - SINGLE BLINK pattern (Version 1.0.0)");
    
    int seconds_counter = 0;
    while (1) {
        // Version 1.0.0: CLEAR SINGLE BLINK pattern
        // Pattern: ON for 500ms, OFF for 2500ms (3 second total cycle)
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(500 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(2500 / portTICK_PERIOD_MS);
        
        seconds_counter++;
        
        // Check for updates every 30 seconds
        if (seconds_counter % UPDATE_CHECK_INTERVAL_SECONDS == 0) {
            ESP_LOGI(TAG, "Periodic update check...");
            if (should_update()) {
                ESP_LOGI(TAG, "Update found! Starting OTA...");
                perform_ota_update();
            }
        }
        
        if (seconds_counter % 30 == 0) {
            ESP_LOGI(TAG, "Status: Version 1.0.0 - Running for %d seconds", seconds_counter);
        }
    }
}

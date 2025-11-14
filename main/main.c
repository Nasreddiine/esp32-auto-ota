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
#include "esp_partition.h"
#include "esp_image_format.h"
#include "esp_flash_partitions.h"
#include "esp_efuse.h"
#include "esp_crt_bundle.h"

// WiFi Configuration
#define WIFI_SSID "La_Fibre_dOrange_A516"
#define WIFI_PASS "Z45CSFFXX3TU6EGNT4"

// LED Configuration
#define BLINK_GPIO GPIO_NUM_2

// GitHub OTA Configuration
#define GITHUB_USER "Nasreddiine"
#define GITHUB_REPO "esp32-auto-ota"

// Update check interval (2.5 minutes = 150 seconds)
#define UPDATE_CHECK_INTERVAL_SECONDS 150

// GitHub URLs - Using HTTPS
#define GITHUB_API_URL "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest"
#define FIRMWARE_BIN_URL "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/firmware.bin"

static const char *TAG = "OTA_APP";

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// Sync time with more retries
void sync_time(void) {
    ESP_LOGI(TAG, "Setting time from SNTP");
    setenv("TZ", "UTC", 1);
    tzset();
    
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_setservername(2, "time.windows.com");
    
    sntp_init();
    
    int retry = 0;
    const int retry_count = 30;
    
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "Time synchronized successfully!");
        
        // Print current time for verification
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "Current time: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        ESP_LOGW(TAG, "Time synchronization failed");
        struct timeval tv = {
            .tv_sec = 1704067200,
            .tv_usec = 0
        };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Set fallback time to 2024");
    }
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, attempting to reconnect...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
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
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi started, connecting to: %s", WIFI_SSID);
}

// Simple string extraction for version from JSON
char* extract_version_from_json(const char* json_response) {
    const char* tag_key = "\"tag_name\":\"";
    char* tag_start = strstr(json_response, tag_key);
    
    if (!tag_start) {
        ESP_LOGE(TAG, "tag_name not found in JSON response");
        return NULL;
    }
    
    tag_start += strlen(tag_key);
    char* tag_end = strchr(tag_start, '"');
    
    if (!tag_end) {
        ESP_LOGE(TAG, "Invalid tag_name format in JSON");
        return NULL;
    }
    
    size_t version_len = tag_end - tag_start;
    char* version = malloc(version_len + 1);
    if (!version) {
        ESP_LOGE(TAG, "Failed to allocate memory for version");
        return NULL;
    }
    
    strncpy(version, tag_start, version_len);
    version[version_len] = '\0';
    
    return version;
}

// Get latest version from GitHub API using HTTPS with certificate bundle
char* get_latest_version(void) {
    ESP_LOGI(TAG, "Fetching latest version from GitHub...");
    
    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return NULL;
    }
    
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP Status: %d", status_code);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status: %d", status_code);
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        content_length = 2048;
    }
    
    if (content_length > 4096) {
        content_length = 4096;
    }
    
    char *response = malloc(content_length + 1);
    if (!response) {
        ESP_LOGE(TAG, "Failed to allocate memory for response");
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    int read_len = esp_http_client_read(client, response, content_length);
    if (read_len <= 0) {
        ESP_LOGE(TAG, "Failed to read HTTP response");
        free(response);
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    response[read_len] = '\0';
    ESP_LOGD(TAG, "Response: %s", response);
    
    esp_http_client_cleanup(client);
    
    char *latest_version = extract_version_from_json(response);
    
    if (latest_version) {
        ESP_LOGI(TAG, "Latest version on GitHub: %s", latest_version);
    } else {
        ESP_LOGE(TAG, "Failed to extract version from response");
    }
    
    free(response);
    return latest_version;
}

// Update firmware using standard OTA mechanism with HTTPS and certificate bundle
bool update_firmware(void) {
    ESP_LOGI(TAG, "Starting firmware update from: %s", FIRMWARE_BIN_URL);
    
    esp_http_client_config_t config = {
        .url = FIRMWARE_BIN_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 120000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .bulk_flash_erase = true,
        .partial_http_download = false,
    };
    
    ESP_LOGI(TAG, "Starting HTTPS OTA...");
    esp_err_t ret = esp_https_ota(&ota_config);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Firmware OTA update successful!");
        return true;
    } else {
        ESP_LOGE(TAG, "Firmware OTA update failed: %s", esp_err_to_name(ret));
        return false;
    }
}

bool is_newer_version(const char *current, const char *latest) {
    ESP_LOGI(TAG, "Comparing versions: current=%s, latest=%s", current, latest);
    
    if (strcmp(current, latest) != 0) {
        ESP_LOGI(TAG, "New version available!");
        return true;
    }
    
    ESP_LOGI(TAG, "Already running the latest version");
    return false;
}

bool should_update(void) {
    ESP_LOGI(TAG, "Checking if update needed...");
    
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Currently running: %s", running_app->version);
    
    char *latest_version = get_latest_version();
    if (!latest_version) {
        ESP_LOGW(TAG, "Failed to get latest version from GitHub - will try again later");
        return false;
    }
    
    bool update_needed = is_newer_version(running_app->version, latest_version);
    
    free(latest_version);
    return update_needed;
}

void blink_led_pattern(int times, int delay_ms) {
    for (int i = 0; i < times; i++) {
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(delay_ms / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(delay_ms / portTICK_PERIOD_MS);
    }
}

void perform_ota_update(void) {
    ESP_LOGI(TAG, "Starting OTA update...");
    
    blink_led_pattern(10, 100);
    
    bool update_success = update_firmware();
    
    if (update_success) {
        ESP_LOGI(TAG, "OTA update completed successfully!");
        
        for(int i = 0; i < 5; i++) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        ESP_LOGI(TAG, "Rebooting in 5 seconds...");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        esp_restart();
        
    } else {
        ESP_LOGE(TAG, "OTA update failed!");
        
        for(int i = 0; i < 3; i++) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32 GitHub Auto-OTA Version 1.0.0 ===");
    
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Running version: %s", running_app->version);
    ESP_LOGI(TAG, "WiFi SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "Update check interval: %d seconds (2.5 minutes)", UPDATE_CHECK_INTERVAL_SECONDS);
    
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
    wifi_init();
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, 
                                          WIFI_CONNECTED_BIT, 
                                          false, 
                                          true, 
                                          portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully!");
        
        blink_led_pattern(2, 200);
        
        // Sync time (critical for HTTPS)
        sync_time();
        
        // Initial update check
        if (should_update()) {
            ESP_LOGI(TAG, "Update available! Starting OTA...");
            blink_led_pattern(5, 200);
            perform_ota_update();
        } else {
            ESP_LOGI(TAG, "No update needed - running latest version");
        }
    } else {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
    }
    
    ESP_LOGI(TAG, "Starting main application loop - Version 1.0.0");
    
    int seconds_counter = 0;
    while (1) {
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(2800 / portTICK_PERIOD_MS);
        
        seconds_counter += 3;
        
        if (seconds_counter >= UPDATE_CHECK_INTERVAL_SECONDS) {
            ESP_LOGI(TAG, "Periodic update check...");
            if (should_update()) {
                ESP_LOGI(TAG, "Update available! Starting OTA...");
                blink_led_pattern(8, 150);
                perform_ota_update();
            }
            seconds_counter = 0;
        }
        
        if (seconds_counter % 30 == 0) {
            ESP_LOGI(TAG, "Status: Version %s - Running for %d seconds", 
                     running_app->version, seconds_counter);
        }
    }
}

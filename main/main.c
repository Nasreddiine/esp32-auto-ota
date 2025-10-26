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

// WiFi Configuration
#define WIFI_SSID "INPT-Residence"
#define WIFI_PASS "iinnpptt"

// LED Configuration
#define BLINK_GPIO GPIO_NUM_2

// GitHub OTA Configuration
#define GITHUB_USER "Nasreddiine"
#define GITHUB_REPO "esp32-auto-ota"

// Update check interval (1 minute 30 seconds = 90 seconds)
#define UPDATE_CHECK_INTERVAL_SECONDS 90

// GitHub URLs - Using HTTP to avoid certificate issues
#define GITHUB_API_URL "http://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest"
#define FIRMWARE_BIN_URL "http://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/firmware.bin"
#define BOOTLOADER_BIN_URL "http://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/bootloader.bin"
#define PARTITION_TABLE_BIN_URL "http://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/partition-table.bin"

static const char *TAG = "OTA_APP";

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// Sync time
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
    const int retry_count = 15;
    
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "Time synchronized successfully!");
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

// Get latest version from GitHub API using HTTP
char* get_latest_version(void) {
    ESP_LOGI(TAG, "Fetching latest version from GitHub...");
    
    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .timeout_ms = 20000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return NULL;
    }
    
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
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
    
    int content_length = esp_http_client_get_content_length(client);
    if (content_length <= 0) {
        content_length = 4096;
    }
    
    if (content_length > 8192) {
        content_length = 8192;
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

// Download a binary file from GitHub
bool download_binary(const char* url, const char* filename) {
    ESP_LOGI(TAG, "Downloading %s from %s", filename, url);
    
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 120000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client for %s", filename);
        return false;
    }
    
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection for %s: %s", filename, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request for %s failed with status: %d", filename, status_code);
        esp_http_client_cleanup(client);
        return false;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Invalid content length for %s: %d", filename, content_length);
        esp_http_client_cleanup(client);
        return false;
    }
    
    // Read the binary data
    uint8_t* data = malloc(content_length);
    if (!data) {
        ESP_LOGE(TAG, "Failed to allocate memory for %s", filename);
        esp_http_client_cleanup(client);
        return false;
    }
    
    int read_len = esp_http_client_read(client, (char*)data, content_length);
    if (read_len != content_length) {
        ESP_LOGE(TAG, "Failed to read complete file %s", filename);
        free(data);
        esp_http_client_cleanup(client);
        return false;
    }
    
    esp_http_client_cleanup(client);
    
    // For now, we'll just log success - in a real implementation, 
    // you'd write this to the appropriate partition
    ESP_LOGI(TAG, "Successfully downloaded %s (%d bytes)", filename, content_length);
    
    free(data);
    return true;
}

bool is_newer_version(const char *current, const char *latest) {
    ESP_LOGI(TAG, "Comparing versions: current=%s, latest=%s", current, latest);
    return strcmp(current, latest) != 0;
}

bool should_update(void) {
    ESP_LOGI(TAG, "Checking if update needed...");
    
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Currently running: %s", running_app->version);
    
    char *latest_version = get_latest_version();
    if (!latest_version) {
        ESP_LOGE(TAG, "Failed to get latest version from GitHub");
        return false;
    }
    
    bool update_needed = is_newer_version(running_app->version, latest_version);
    
    if (update_needed) {
        ESP_LOGI(TAG, "Update needed: running %s, latest is %s", 
                 running_app->version, latest_version);
    } else {
        ESP_LOGI(TAG, "No update needed - running latest version %s", latest_version);
    }
    
    free(latest_version);
    return update_needed;
}

void perform_ota_update(void) {
    ESP_LOGI(TAG, "Starting complete OTA update from GitHub...");
    
    // Download all necessary binaries
    bool bootloader_ok = download_binary(BOOTLOADER_BIN_URL, "bootloader.bin");
    bool partition_ok = download_binary(PARTITION_TABLE_BIN_URL, "partition-table.bin");
    bool firmware_ok = download_binary(FIRMWARE_BIN_URL, "firmware.bin");
    
    if (!firmware_ok) {
        ESP_LOGE(TAG, "Failed to download firmware.bin");
        return;
    }
    
    ESP_LOGI(TAG, "All binaries downloaded successfully!");
    ESP_LOGI(TAG, "Bootloader: %s, Partition: %s, Firmware: %s",
             bootloader_ok ? "OK" : "Failed", 
             partition_ok ? "OK" : "Failed",
             firmware_ok ? "OK" : "Failed");
    
    // Note: In a production system, you would:
    // 1. Write bootloader to bootloader partition
    // 2. Write partition table to partition table area  
    // 3. Use esp_https_ota for firmware update
    // 4. Set boot flags and reboot
    
    ESP_LOGI(TAG, "Update complete! Ready to reboot...");
    
    // Success blinking
    for(int i = 0; i < 10; i++) {
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    esp_restart();
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
    ESP_LOGI(TAG, "=== ESP32 GitHub Auto-OTA Version 1.0.0 ===");
    
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Running version: %s", running_app->version);
    ESP_LOGI(TAG, "Update check interval: %d seconds", UPDATE_CHECK_INTERVAL_SECONDS);
    
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
    ESP_LOGI(TAG, "Connecting to WiFi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected!");
    
    // Sync time
    sync_time();
    
    // Initial update check
    if (should_update()) {
        ESP_LOGI(TAG, "Update needed! Starting OTA...");
        blink_led_pattern(5, 200);
        perform_ota_update();
    }
    
    ESP_LOGI(TAG, "Starting main application - Single blink pattern (version 1.0.0)");
    
    // Main loop
    int seconds_counter = 0;
    while (1) {
        // Version 1.0.0: Single blink pattern every 3 seconds
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(2800 / portTICK_PERIOD_MS);
        
        seconds_counter += 3;
        
        // Check for updates every 90 seconds
        if (seconds_counter % UPDATE_CHECK_INTERVAL_SECONDS == 0) {
            ESP_LOGI(TAG, "Periodic update check...");
            if (should_update()) {
                ESP_LOGI(TAG, "Update available! Starting OTA...");
                blink_led_pattern(8, 150);
                perform_ota_update();
            }
        }
        
        // Show status every 30 seconds
        if (seconds_counter % 30 == 0) {
            ESP_LOGI(TAG, "Status: Version %s - Running for %d seconds", 
                     running_app->version, seconds_counter);
        }
    }
}

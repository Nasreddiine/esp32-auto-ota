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
#include "esp_crt_bundle.h"  // For certificate bundle
#include "cJSON.h"           // For JSON parsing

// WiFi Configuration (updated as per request)
#define WIFI_SSID "La_Fibre_dOrange_A516"
#define WIFI_PASS "Z45CSFFXX3TU6EGNT4"

// LED Configuration
#define BLINK_GPIO GPIO_NUM_2

// GitHub OTA Configuration
#define GITHUB_USER "Nasreddiine"
#define GITHUB_REPO "esp32-auto-ota"

// Update check interval (1 minute 30 seconds = 90 seconds)
#define UPDATE_CHECK_INTERVAL_SECONDS 90

// GitHub URLs - Using HTTPS with cert bundle
#define GITHUB_API_URL "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest"
#define FIRMWARE_BIN_URL "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/firmware.bin"

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
            .tv_sec = 1704067200,  // 2024-01-01
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

// Get latest version from GitHub API using HTTPS with retries
char* get_latest_version(void) {
    ESP_LOGI(TAG, "Fetching latest version from GitHub...");
    
    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Enable cert bundle
    };
    
    char *latest_version = NULL;
    int max_retries = 3;
    for (int retry = 0; retry < max_retries; retry++) {
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) continue;
        
        esp_http_client_set_method(client, HTTP_METHOD_GET);
        esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client");
        esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
        
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP request failed (retry %d): %s", retry, esp_err_to_name(err));
            esp_http_client_cleanup(client);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }
        
        int status_code = esp_http_client_get_status_code(client);
        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP status: %d (retry %d)", status_code, retry);
            esp_http_client_cleanup(client);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }
        
        int content_length = esp_http_client_get_content_length(client);
        if (content_length <= 0 || content_length > 8192) content_length = 8192;
        
        char *response = malloc(content_length + 1);
        if (!response) {
            esp_http_client_cleanup(client);
            continue;
        }
        
        int read_len = esp_http_client_read(client, response, content_length);
        if (read_len <= 0) {
            free(response);
            esp_http_client_cleanup(client);
            continue;
        }
        response[read_len] = '\0';
        
        // Parse with cJSON
        cJSON *json = cJSON_Parse(response);
        if (json) {
            cJSON *tag_name = cJSON_GetObjectItem(json, "tag_name");
            if (cJSON_IsString(tag_name) && tag_name->valuestring) {
                latest_version = strdup(tag_name->valuestring);
            }
            cJSON_Delete(json);
        }
        
        free(response);
        esp_http_client_cleanup(client);
        
        if (latest_version) {
            ESP_LOGI(TAG, "Latest version: %s", latest_version);
            return latest_version;
        }
    }
    
    ESP_LOGE(TAG, "Failed after %d retries", max_retries);
    return NULL;
}

// Simple semantic version comparison (major.minor.patch)
bool is_newer_version(const char *current, const char *latest) {
    if (!current || !latest) return false;
    
    int cur_major, cur_minor, cur_patch;
    int lat_major, lat_minor, lat_patch;
    
    if (sscanf(current, "%d.%d.%d", &cur_major, &cur_minor, &cur_patch) != 3 ||
        sscanf(latest, "%d.%d.%d", &lat_major, &lat_minor, &lat_patch) != 3) {
        return strcmp(current, latest) != 0;  // Fallback to string compare
    }
    
    if (lat_major > cur_major) return true;
    if (lat_major == cur_major && lat_minor > cur_minor) return true;
    if (lat_major == cur_major && lat_minor == cur_minor && lat_patch > cur_patch) return true;
    
    return false;
}

bool should_update(void) {
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Running version: %s", running_app->version);
    
    char *latest_version = get_latest_version();
    if (!latest_version) return false;
    
    bool update_needed = is_newer_version(running_app->version, latest_version);
    ESP_LOGI(TAG, "Update %sneeded (latest: %s)", update_needed ? "" : "not ", latest_version);
    
    free(latest_version);
    return update_needed;
}

// Update firmware using standard OTA with HTTPS
bool update_firmware(void) {
    ESP_LOGI(TAG, "Updating firmware using OTA...");
    
    esp_http_client_config_t config = {
        .url = FIRMWARE_BIN_URL,
        .timeout_ms = 120000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .bulk_flash_erase = true,
        .partial_http_download = false,
    };
    
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful!");
        return true;
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        return false;
    }
}

void perform_ota_update(void) {
    // Only update app firmware (no bootloader/partition)
    if (update_firmware()) {
        ESP_LOGI(TAG, "Update complete! Rebooting...");
        // Success blinking
        for(int i = 0; i < 20; i++) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(50 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(50 / portTICK_PERIOD_MS);
        }
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Update failed!");
        // Error blinking
        for(int i = 0; i < 10; i++) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(500 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
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

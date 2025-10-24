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

// Update check interval (1 minute 30 seconds = 90 seconds)
#define UPDATE_CHECK_INTERVAL_SECONDS 90

// GitHub URLs
#define GITHUB_API_URL "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest"
#define FIRMWARE_BIN_URL "https://github.com/" GITHUB_USER "/" GITHUB_REPO "/releases/latest/download/firmware.bin"

static const char *TAG = "OTA_APP";

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// GitHub's root certificate (simplified - for testing)
// In production, you should use the full certificate chain
static const char *github_root_cert = "-----BEGIN CERTIFICATE-----\n" \
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n" \
"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n" \
"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n" \
"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n" \
"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n" \
"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n" \
"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n" \
"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n" \
"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n" \
"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n" \
"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n" \
"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n" \
"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n" \
"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n" \
"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n" \
"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n" \
"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n" \
"rqXRfboQnoZsG4q5WTP468SQvvG5\n" \
"-----END CERTIFICATE-----";

// Sync time for TLS certificate validation
void sync_time(void) {
    ESP_LOGI(TAG, "Setting time from SNTP");
    setenv("TZ", "UTC", 1);
    tzset();
    
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // Add multiple NTP servers for reliability
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_setservername(2, "time.windows.com");
    
    sntp_init();
    
    // Wait for time to be set
    int retry = 0;
    const int retry_count = 10;
    
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "Time synchronized successfully!");
    } else {
        ESP_LOGW(TAG, "Time synchronization failed");
        // Set fallback time
        struct timeval tv = {
            .tv_sec = 1704067200, // January 1, 2024
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

// Get latest version from GitHub API with FIXED TLS configuration
char* get_latest_version(void) {
    ESP_LOGI(TAG, "Fetching latest version from GitHub...");
    
    // First try with proper certificate
    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .timeout_ms = 15000,
        .cert_pem = github_root_cert,
        .skip_cert_common_name_check = false,
        .keep_alive_enable = true,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return NULL;
    }
    
    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        
        // Fallback: try without certificate verification
        ESP_LOGW(TAG, "Trying fallback without certificate...");
        esp_http_client_config_t fallback_config = {
            .url = GITHUB_API_URL,
            .timeout_ms = 15000,
            .cert_pem = NULL,
            .skip_cert_common_name_check = true,
        };
        
        client = esp_http_client_init(&fallback_config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to initialize fallback HTTP client");
            return NULL;
        }
        
        esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client");
        esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
        
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Fallback also failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return NULL;
        }
    }
    
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP Status: %d", status_code);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Invalid content length: %d", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
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
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    response[read_len] = '\0';
    
    esp_http_client_close(client);
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
    ESP_LOGI(TAG, "Starting OTA update from GitHub...");
    
    // Try with certificate first
    esp_http_client_config_t config = {
        .url = FIRMWARE_BIN_URL,
        .timeout_ms = 120000,
        .cert_pem = github_root_cert,
        .skip_cert_common_name_check = false,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .bulk_flash_erase = true,
    };
    
    ESP_LOGI(TAG, "Initializing HTTPS OTA...");
    esp_err_t err = esp_https_ota(&ota_config);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA Update Successful! Rebooting...");
        
        for(int i = 0; i < 10; i++) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Update Failed: %s", esp_err_to_name(err));
        
        // Try fallback without certificate
        ESP_LOGW(TAG, "Trying OTA fallback without certificate...");
        esp_http_client_config_t fallback_config = {
            .url = FIRMWARE_BIN_URL,
            .timeout_ms = 120000,
            .cert_pem = NULL,
            .skip_cert_common_name_check = true,
        };
        
        esp_https_ota_config_t fallback_ota_config = {
            .http_config = &fallback_config,
            .bulk_flash_erase = true,
        };
        
        err = esp_https_ota(&fallback_ota_config);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA Fallback Successful! Rebooting...");
            for(int i = 0; i < 15; i++) {
                gpio_set_level(BLINK_GPIO, 1);
                vTaskDelay(100 / portTICK_PERIOD_MS);
                gpio_set_level(BLINK_GPIO, 0);
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA Fallback also failed: %s", esp_err_to_name(err));
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
    ESP_LOGI(TAG, "=== ESP32 GitHub Auto-OTA ===");
    
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
    
    // Sync time for TLS
    sync_time();
    
    // Initial update check
    if (should_update()) {
        ESP_LOGI(TAG, "Update needed! Starting OTA...");
        blink_led_pattern(5, 200);
        perform_ota_update();
    }
    
    ESP_LOGI(TAG, "Starting main application - LED double blink pattern (version 1.0.2)");
    
    // Main loop
    int seconds_counter = 0;
    while (1) {
        // Version 1.0.2: Double blink pattern
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(2400 / portTICK_PERIOD_MS);
        
        seconds_counter++;
        
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

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

// Update check interval (1 minute 30 seconds = 90 seconds)
#define UPDATE_CHECK_INTERVAL_SECONDS 90
#define UPDATE_CHECK_CYCLES (UPDATE_CHECK_INTERVAL_SECONDS * 2) // Since each cycle is 500ms

// GitHub URLs
#define GITHUB_API_URL "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest"
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

// Simple string extraction for version from JSON
char* extract_version_from_json(const char* json_response) {
    const char* tag_key = "\"tag_name\":\"";
    char* tag_start = strstr(json_response, tag_key);
    
    if (!tag_start) {
        ESP_LOGE(TAG, "tag_name not found in JSON response");
        return NULL;
    }
    
    tag_start += strlen(tag_key); // Move past the key
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

// Get latest version from GitHub API
char* get_latest_version(void) {
    ESP_LOGI(TAG, "Fetching latest version from GitHub...");
    
    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .timeout_ms = 15000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return NULL;
    }
    
    // Set User-Agent header (GitHub requires this)
    esp_http_client_set_header(client, "User-Agent", "ESP32-OTA-Client");
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Invalid content length: %d", content_length);
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    // Limit response size to prevent memory issues
    if (content_length > 4096) {
        ESP_LOGW(TAG, "Response too large, limiting to 4096 bytes");
        content_length = 4096;
    }
    
    char *response = malloc(content_length + 1);
    if (!response) {
        ESP_LOGE(TAG, "Failed to allocate memory for response");
        esp_http_client_cleanup(client);
        return NULL;
    }
    
    int read_len = esp_http_client_read(client, response, content_length);
    response[read_len] = '\0';
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    // Extract version using simple string parsing
    char *latest_version = extract_version_from_json(response);
    
    if (latest_version) {
        ESP_LOGI(TAG, "Latest version on GitHub: %s", latest_version);
    } else {
        ESP_LOGE(TAG, "Failed to extract version from response");
    }
    
    free(response);
    return latest_version;
}

// Compare version strings
bool is_newer_version(const char *current, const char *latest) {
    ESP_LOGI(TAG, "Comparing versions: current=%s, latest=%s", current, latest);
    return strcmp(current, latest) != 0;
}

bool should_update(void) {
    ESP_LOGI(TAG, "Checking if update needed...");
    
    // Get current running firmware info
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Currently running: %s", running_app->version);
    
    // Get latest version from GitHub
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
    ESP_LOGI(TAG, "Download URL: %s", FIRMWARE_BIN_URL);
    
    esp_http_client_config_t config = {
        .url = FIRMWARE_BIN_URL,
        .timeout_ms = 120000, // 120 seconds for larger firmware
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .bulk_flash_erase = true,
        .partial_http_download = false,
    };
    
    ESP_LOGI(TAG, "Initializing HTTPS OTA...");
    esp_https_ota_handle_t https_ota_handle = NULL;
    
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed: %s", esp_err_to_name(err));
        return;
    }
    
    ESP_LOGI(TAG, "ESP HTTPS OTA Begin successful");
    
    // Perform OTA download
    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        ESP_LOGI(TAG, "Downloading...");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Download completed, finalizing...");
        err = esp_https_ota_finish(https_ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA Update Successful! Rebooting in 5 seconds...");
            
            // Blink rapidly to indicate success
            for(int i = 0; i < 15; i++) {
                gpio_set_level(BLINK_GPIO, 1);
                vTaskDelay(100 / portTICK_PERIOD_MS);
                gpio_set_level(BLINK_GPIO, 0);
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA Finish failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "OTA Perform failed: %s", esp_err_to_name(err));
    }
    
    if (https_ota_handle) {
        esp_https_ota_abort(https_ota_handle);
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
    
    // Get current version
    const esp_app_desc_t *running_app = esp_ota_get_app_description();
    ESP_LOGI(TAG, "Running version: %s", running_app->version);
    ESP_LOGI(TAG, "Repo: %s/%s", GITHUB_USER, GITHUB_REPO);
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
    ESP_LOGI(TAG, "LED initialized on GPIO %d", BLINK_GPIO);
    
    // Connect to WiFi
    wifi_init();
    ESP_LOGI(TAG, "Connecting to WiFi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected!");
    
    // Initial update check
    if (should_update()) {
        ESP_LOGI(TAG, "Update needed! Starting OTA process...");
        blink_led_pattern(5, 200); // Fast blink = updating
        perform_ota_update();
    } else {
        ESP_LOGI(TAG, "Running latest version %s", running_app->version);
    }
    
    ESP_LOGI(TAG, "Starting main application");
    
    // Main application loop
    int counter = 0;
    while (1) {
        // Normal operation - slow blink
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        
        counter++;
        
        // Check for updates every 1 minute 30 seconds (90 seconds)
        if (counter % UPDATE_CHECK_CYCLES == 0) {
            ESP_LOGI(TAG, "Periodic update check (every %d seconds)...", UPDATE_CHECK_INTERVAL_SECONDS);
            if (should_update()) {
                ESP_LOGI(TAG, "Update available! Starting OTA...");
                blink_led_pattern(8, 150);
                perform_ota_update();
            }
            // Reset counter periodically to avoid overflow
            if (counter >= 10000) {
                counter = 0;
                ESP_LOGI(TAG, "Counter reset to prevent overflow");
            }
        }
        
        // Show status every 30 cycles (15 seconds)
        if (counter % 30 == 0) {
            ESP_LOGI(TAG, "Status: Running version %s - Total cycles: %d", 
                     running_app->version, counter);
            int cycles_until_next_check = UPDATE_CHECK_CYCLES - (counter % UPDATE_CHECK_CYCLES);
            ESP_LOGI(TAG, "Next update check in: %d cycles (%d seconds)", 
                     cycles_until_next_check, cycles_until_next_check / 2);
        }
    }
}


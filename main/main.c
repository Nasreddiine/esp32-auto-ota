#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "wifi_config.h"
#include "ota_manager.h"

static const char *TAG = "MAIN";

// WiFi event group
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// Timer for update checks
TimerHandle_t update_timer;

// WiFi event handler
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

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "📡 WiFi initialization finished.");
}

// Update check timer callback
void update_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "⏰ Scheduled update check...");
    ota_check_update();
    
    if (ota_is_update_available()) {
        ESP_LOGI(TAG, "🔄 Update available! Performing update...");
        ota_perform_update();
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "🚀 ESP32 Auto-OTA Project Starting...");
    ESP_LOGI(TAG, "🏷️ Firmware Version: %s", ota_get_current_version());

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize OTA manager
    ota_init();

    // Initialize WiFi
    wifi_init();

    // Wait for WiFi connection
    ESP_LOGI(TAG, "⌛ Waiting for WiFi connection...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, " WiFi Connected!");

    // Create timer for periodic update checks (every 10 minutes)
    update_timer = xTimerCreate(
        "UpdateCheck",
        pdMS_TO_TICKS(10 * 60 * 1000),  // 10 minutes
        pdTRUE,                         // Auto-reload
        (void *)0,
        update_timer_callback
    );
    
    xTimerStart(update_timer, 0);
    ESP_LOGI(TAG, "Update checker started (10 minute intervals)");

    // Check for updates immediately (after 30 seconds)
    ESP_LOGI(TAG, " Initial update check in 30 seconds...");
    vTaskDelay(30000 / portTICK_PERIOD_MS);
    ota_check_update();

    // Main application loop
    int counter = 0;
    while(1) {
        ESP_LOGI(TAG, "🔧 Main loop running... Cycle: %d, Free memory: %d bytes", 
                counter++, esp_get_free_heap_size());
        
        // Check if update is available and perform it
        if (ota_is_update_available()) {
            ESP_LOGI(TAG, " Update detected! Performing update...");
            ota_perform_update();
        }
        
        vTaskDelay(30000 / portTICK_PERIOD_MS);  // 30 seconds
    }
}
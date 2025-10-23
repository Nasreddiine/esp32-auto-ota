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
#include "nvs_flash.h"
#include "driver/gpio.h"

// SET YOUR WIFI HERE
#define WIFI_SSID "INPT-Residence"
#define WIFI_PASS "iinnpptt"

// Try different GPIOs - start with GPIO2
#define BLINK_GPIO GPIO_NUM_2

static const char *TAG = "OTA_APP";
#define FIRMWARE_VERSION "1.0.0"

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

void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP32 Auto-OTA - Version: %s", FIRMWARE_VERSION);
    
    // Initialize NVS (simplified)
    nvs_flash_init();
    
    // Initialize LED GPIO
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    ESP_LOGI(TAG, "LED ready on GPIO %d", BLINK_GPIO);
    
    // Start WiFi (don't wait for connection)
    wifi_init();
    ESP_LOGI(TAG, "WiFi connecting...");
    
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
        
        // Show status every 10 cycles
        if (counter % 10 == 0) {
            ESP_LOGI(TAG, "Still running - Total cycles: %d", counter);
        }
    }
}

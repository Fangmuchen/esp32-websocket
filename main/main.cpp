#include "globals.h"
#include "led_control.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "http_server.h"
#include "websocket_client.h"
#include "device_control.h"
#include <cstring>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ledctrl";

device_state_t s_device_state = STATE_BOOT;
static int64_t s_ap_start_us = 0;

// ================================================================
//  主任务（状态机）
// ================================================================

static void main_task(void *arg)
{
    // ---- 启动提示 ----
    blink_led(2, 200);

    // ---- 检测 BOOT 按键：按住 3 秒恢复出厂 ----
    gpio_set_direction(BTN_CFG_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_CFG_PIN, GPIO_PULLUP_ONLY);

    if (gpio_get_level(BTN_CFG_PIN) == 0) {
        ESP_LOGI(TAG, "BOOT pressed, hold 3s for reset...");
        bool do_reset = true;
        for (int i = 0; i < 30; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(BTN_CFG_PIN) != 0) { do_reset = false; break; }
        }
        if (do_reset) {
            ESP_LOGI(TAG, "Factory reset!");
            clear_config();
            blink_led(6, 150);
        }
    }

    // ---- 创建 WiFi netif ----
    esp_netif_create_default_wifi_sta();

    // ---- 初始化设备控制 ----
    control_init();

    // ---- 尝试加载配置并连接 WiFi ----
    if (load_config()) {
        if (connect_to_wifi(s_saved_ssid, s_saved_pass)) {
            connect_websocket();
            goto state_loop;
        }
    }

    // ---- AP 配网模式 ----
    ESP_LOGI(TAG, "Enter AP config mode");
    blink_led(5, 300);
    start_ap_mode();
    s_ap_start_us = esp_timer_get_time();
    start_http_server();

state_loop:
    while (1) {
        switch (s_device_state) {

        case STATE_AP_CONFIG: {
            if ((esp_timer_get_time() - s_ap_start_us) > AP_IDLE_TIMEOUT_US) {
                ESP_LOGI(TAG, "AP idle timeout, reboot");
                esp_restart();
            }
            static int64_t last_blink = 0;
            int64_t now = esp_timer_get_time();
            if (now - last_blink > 1000000) {
                static int phase = 0;
                ws2812_set_color(0, 0, (++phase % 2) ? 50 : 0);
                last_blink = now;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }

        case STATE_ONLINE:
            ws_tick();
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;

        default:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

// ================================================================
//  app_main — ESP-IDF 入口
// ================================================================

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    wifi_init_event_group();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               wifi_event_handler, NULL);

    ESP_ERROR_CHECK(led_init());
    set_led(false);

    xTaskCreate(main_task, "main", 4096, NULL, 5, NULL);
}

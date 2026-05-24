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

/*
 * main.cpp — 程序入口和主状态机 v2.0
 * ===================================
 *
 * v2.0 优化：
 *   1. 支持 WiFi 断线后自动重连（在 STATE_ONLINE 中检测并切换回 CONNECTING）
 *   2. 可选 NTP 时间同步（通过 globals.h 中 ENABLE_NTP 控制）
 *   3. 移除 token 校验相关代码
 */

static const char *TAG = "ledctrl";

device_state_t s_device_state = STATE_BOOT;
static int64_t s_ap_start_us = 0;

// 用于 WiFi 断线检测的定时器
static int64_t s_last_wifi_check = 0;
static bool s_wifi_reconnect_pending = false;

#ifdef ENABLE_NTP
#include "esp_sntp.h"

static void ntp_sync_time(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER1);
    esp_sntp_setservername(1, NTP_SERVER2);
    esp_sntp_init();

    // 等待时间同步（最多 10 秒）
    time_t now = 0;
    int retry = 0;
    while (now < 100000 && retry < 10) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        retry++;
    }
    if (now > 100000) {
        struct tm *tm = localtime(&now);
        ESP_LOGI(TAG, "NTP synchronized: %04d-%02d-%02d %02d:%02d:%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
        // 设置时区
        setenv("TZ", NTP_TIMEZONE, 1);
        tzset();
    } else {
        ESP_LOGW(TAG, "NTP sync timeout, using default time");
    }
}
#endif

// ========== 主任务 ==========
static void main_task(void *arg)
{
    blink_led(2, 200);

    // BOOT 按键检测（按住 3 秒恢复出厂设置）
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

    esp_netif_create_default_wifi_sta();
    control_init();

    // 尝试加载配置并连接 WiFi
    if (load_config()) {
        if (connect_to_wifi(s_saved_ssid, s_saved_pass)) {
#ifdef ENABLE_NTP
            ntp_sync_time();
#endif
            connect_websocket();
            goto state_loop;
        }
    }

    // AP 配网模式
    ESP_LOGI(TAG, "Enter AP config mode");
    blink_led(5, 300);
    start_ap_mode();
    s_ap_start_us = esp_timer_get_time();
    start_http_server();

state_loop:
    while (1) {
        switch (s_device_state) {

        case STATE_AP_CONFIG:
            // AP 配网模式：超时自动重启
            if ((esp_timer_get_time() - s_ap_start_us) > AP_IDLE_TIMEOUT_US) {
                ESP_LOGI(TAG, "AP idle timeout, reboot");
                esp_restart();
            }
            // LED 慢闪
            {
                static int64_t last_blink = 0;
                int64_t now = esp_timer_get_time();
                if (now - last_blink > 1000000) {
                    static int phase = 0;
                    ws2812_set_color(0, 0, (++phase % 2) ? 50 : 0);
                    last_blink = now;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case STATE_CONNECTING:
            // 连接中，等待 wifi_manager 回调设置状态
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case STATE_ONLINE: {
            // —— 在线模式 ——
            // 1) 检查 WiFi 是否仍在线
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
                // WiFi 已断线
                if (!s_wifi_reconnect_pending) {
                    ESP_LOGW(TAG, "WiFi disconnected, will reconnect...");
                    s_wifi_reconnect_pending = true;
                    s_device_state = STATE_CONNECTING;
                    esp_wifi_connect();
                }
            } else {
                s_wifi_reconnect_pending = false;
            }

            // 2) 发送心跳
            ws_tick();
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }

        default:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

// ================================================================
//  app_main — ESP-IDF 程序入口
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

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
 * main.cpp — 程序入口和主状态机
 * ================================
 *
 * 【程序入口】
 * app_main() 是 ESP-IDF 的入口函数（相当于普通 C 程序的 main()）。
 * 它在 ESP32 启动后自动被调用。
 *
 * 【整体流程】
 * 上电 → NVS 初始化 → WiFi 初始化 → LED 初始化 → 创建主任务（main_task）
 *   ├─ 检测 BOOT 按键是否按住 3 秒 → 是 → 恢复出厂设置
 *   ├─ 尝试加载 WiFi 配置 → 有配置 → 连接 WiFi
 *   │   ├─ 连接成功 → 连接 WebSocket → STATE_ONLINE 循环
 *   │   └─ 连接失败 → 进入 AP 配网模式
 *   └─ 没有配置 → 进入 AP 配网模式
 *       └─ AP 模式 → 启动 HTTP 服务器 + DNS 服务器 → 等待用户配网
 *                     → 用户提交配置 → 保存到 NVS → 重启
 *
 * 【状态机】
 *   STATE_BOOT       — 启动初始化中
 *   STATE_AP_CONFIG  — AP 配网模式，ESP32 发射热点，等待手机配网
 *   STATE_CONNECTING — 正在连接 WiFi
 *   STATE_ONLINE     — 在线工作：定时发送心跳、接收控制指令
 */

static const char *TAG = "ledctrl";

device_state_t s_device_state = STATE_BOOT;  // 全局设备状态
static int64_t s_ap_start_us = 0;            // 进入 AP 模式时的时间戳（用于超时判断）

// ================================================================
//  主任务（状态机 + 流程控制）
//  这是一个独立的 FreeRTOS 任务，栈大小 4096 字节
// ================================================================

static void main_task(void *arg)
{
    // ---- 启动提示：LED 闪烁 2 次 ----
    blink_led(2, 200);

    // ---- 检测 BOOT 按键是否按住 3 秒 ----
    //   如果按住不放 3 秒 → 清除 NVS 中保存的 WiFi 配置 → 恢复出厂
    //   这是用户唯一的不需要连电脑就能重置设备的方法
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
            clear_config();                 // 清除 NVS 中的 WiFi 配置
            blink_led(6, 150);             // 闪烁 6 次提示重置完成
        }
    }

    // ---- 创建 WiFi STA 网络接口 ----
    esp_netif_create_default_wifi_sta();

    // ---- 初始化设备控制 ----
    control_init();

    // ---- 尝试加载配置并连接 WiFi ----
    if (load_config()) {
        if (connect_to_wifi(s_saved_ssid, s_saved_pass)) {
            connect_websocket();   // WiFi 连接成功 → 启动 WebSocket
            goto state_loop;       // 直接进入主循环
        }
    }

    // ---- AP 配网模式（连接 WiFi 失败或没有配置）----
    ESP_LOGI(TAG, "Enter AP config mode");
    blink_led(5, 300);             // 闪烁 5 次提示进入配网模式
    start_ap_mode();               // 启动 WiFi 热点
    s_ap_start_us = esp_timer_get_time();  // 记录进入 AP 模式的时间
    start_http_server();           // 启动 HTTP 配网页面

    // ================================================================
    //  主状态循环
    //  根据 s_device_state 执行不同的逻辑
    // ================================================================
state_loop:
    while (1) {
        switch (s_device_state) {

        case STATE_AP_CONFIG: {
            // —— AP 配网模式：等待用户配网，超时自动重启 ——
            if ((esp_timer_get_time() - s_ap_start_us) > AP_IDLE_TIMEOUT_US) {
                ESP_LOGI(TAG, "AP idle timeout, reboot");
                esp_restart();
            }
            // LED 慢闪，提示正在配网模式
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
            // —— 在线模式：定时发送心跳包 ——
            ws_tick();              // 自动计时发送 WebSocket 心跳
            vTaskDelay(pdMS_TO_TICKS(1000));  // 每秒检查一次
            break;

        default:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

// ================================================================
//  app_main — ESP-IDF 程序入口
//  ESP32 启动后自动调用此函数
// ================================================================

extern "C" void app_main(void)
{
    // 1) 初始化 NVS 闪存存储
    //    NVS 用于保存 WiFi 配置和其他持久化数据
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());  // NVS 损坏或版本不兼容，擦除重试
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2) 初始化网络接口和事件循环
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3) 初始化 WiFi 驱动（此时只创建驱动并不启动 WiFi）
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    // 4) 创建 WiFi 事件组，注册 WiFi 事件处理函数
    wifi_init_event_group();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               wifi_event_handler, NULL);

    // 5) 初始化 WS2812 LED
    ESP_ERROR_CHECK(led_init());
    set_led(false);  // 初始状态：灯灭

    // 6) 创建主任务（main_task），栈大小 4096，优先级 5
    xTaskCreate(main_task, "main", 4096, NULL, 5, NULL);
}

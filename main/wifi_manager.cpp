#include "globals.h"
#include "wifi_manager.h"
#include <cstring>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/*
 * wifi_manager.cpp — WiFi 连接与热点模式（实现）
 * ==============================================
 *
 * 【STA 模式连接流程】
 * main_task → 调用 connect_to_wifi() → 设置 WiFi 配置 → 启动 WiFi → 连接
 * → 等待事件组（WIFI_CONNECTED_BIT / WIFI_FAIL_BIT）→ 返回成功/失败
 *
 * 【AP 模式启动流程】
 * main_task → 调用 start_ap_mode() → 停止 WiFi → 设为 APSTA 模式
 * → 配置热点 SSID（MAC 后缀）→ 启动 WiFi → 手机可见
 *
 * 【事件驱动】
 * ESP32 的 WiFi 是事件驱动的。我们注册了一个事件处理函数：
 *   - WIFI_EVENT_STA_DISCONNECTED → 连接失败/断开 → 设置 WIFI_FAIL_BIT
 *   - IP_EVENT_STA_GOT_IP → 成功获取 IP → 设置 WIFI_CONNECTED_BIT
 * connect_to_wifi() 通过等待这些事件位来判断连接结果
 */

static const char *TAG = "ledctrl";

char s_ap_ssid[32];                        // AP 模式的热点名称
static EventGroupHandle_t s_wifi_event_group = NULL;  // WiFi 事件组

// ================================================================
//  WiFi 事件处理函数
//  这是回调函数，WiFi 模块在发生事件时会自动调用它
// ================================================================
void wifi_event_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // STA 断开连接（可能是密码错误、路由器没开、信号差等）
        extern device_state_t s_device_state;
        if (s_device_state == STATE_CONNECTING)
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        // 成功获取到 IP 地址，说明 WiFi 连接成功
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ================================================================
//  获取 WiFi 事件组句柄（给其他模块使用）
// ================================================================
EventGroupHandle_t wifi_get_event_group(void)
{
    return s_wifi_event_group;
}

// ================================================================
//  创建 WiFi 事件组（在 app_main 中调用）
// ================================================================
void wifi_init_event_group(void)
{
    s_wifi_event_group = xEventGroupCreate();
}

// ================================================================
//  连接 WiFi（STA 模式）
//  参数：
//    ssid — WiFi 名称
//    pass — WiFi 密码
//  返回：
//    true  = 连接成功，设备进入 STATE_ONLINE
//    false = 连接超时/失败
// ================================================================
bool connect_to_wifi(const char *ssid, const char *pass)
{
    extern device_state_t s_device_state;

    ESP_LOGI(TAG, "Connecting to %s ...", ssid);
    s_device_state = STATE_CONNECTING;

    // 配置 WiFi
    wifi_config_t cfg = {};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t conn_err = esp_wifi_connect();
    if (conn_err != ESP_OK) {
        ESP_LOGW(TAG, "Connect failed: %s", esp_err_to_name(conn_err));
        s_device_state = STATE_BOOT;
        return false;
    }

    // 等待事件组信号：连接成功或失败（最多等 WIFI_TIMEOUT_MS 毫秒）
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdTRUE, pdFALSE, pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi OK");
        s_device_state = STATE_ONLINE;
        return true;
    }
    ESP_LOGW(TAG, "WiFi timeout");
    return false;  // 连接失败，main_task 会进入 AP 配网模式
}

// ================================================================
//  启动 AP 热点模式（配网用）
//  热点名称 = AP_SSID_PREFIX + "-" + MAC 地址后 3 字节
//  手机连接这个热点后，打开浏览器会自动弹出配网页面（DNS 劫持）
// ================================================================
void start_ap_mode(void)
{
    extern device_state_t s_device_state;

    // 用 MAC 地址后 3 位生成唯一的热点名称
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X%02X",
             AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "AP SSID: %s", s_ap_ssid);

    // 创建 AP 网络接口
    esp_netif_create_default_wifi_ap();

    // 先停 WiFi（可能之前以 STA 模式启动过，忽略 NOT_STARTED 错误）
    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_ERROR_CHECK(stop_err);
    }
    // 切换到 APSTA 模式（同时做热点和客户端）
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // 配置 AP
    wifi_config_t ap_cfg = {};
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;   // 开放网络，不需要密码
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;           // 最多 4 台设备同时连接
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    vTaskDelay(pdMS_TO_TICKS(300));         // 等电源稳定
    ESP_ERROR_CHECK(esp_wifi_start());

    s_device_state = STATE_AP_CONFIG;
    ESP_LOGI(TAG, "AP: %s (192.168.4.1)", s_ap_ssid);
}

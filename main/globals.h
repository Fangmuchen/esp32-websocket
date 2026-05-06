#pragma once

#include "driver/gpio.h"

// ========== 硬件配置 ==========
#define LED_PIN             GPIO_NUM_2
#define NUM_LEDS            1
#define BTN_CFG_PIN         GPIO_NUM_0

// ========== 配网配置 ==========
#define AP_SSID_PREFIX      "ESP32-LED"
#define AP_CHANNEL          6
#define WIFI_TIMEOUT_MS     20000
#define AP_IDLE_TIMEOUT_US  300000000LL  // 300s in microseconds

// ========== WebSocket 配置 ==========
#define WS_HOST             "192.168.100.110"
#define WS_PORT             8800
#define WS_PATH             "/"

// ========== NVS ==========
#define NVS_NAMESPACE       "ledctrl"

// ========== WS2812 RMT ==========
#define RMT_RESOLUTION_HZ   10000000

// ========== Event Group Bits ==========
#define WIFI_CONNECTED_BIT  (1 << 0)
#define WIFI_FAIL_BIT       (1 << 1)

// ========== 默认 WiFi 凭据 ==========
#define DEFAULT_SSID        "esp32"
#define DEFAULT_PASS        "123456789012"

// ========== 设备状态 ==========
typedef enum {
    STATE_BOOT,
    STATE_AP_CONFIG,
    STATE_CONNECTING,
    STATE_ONLINE
} device_state_t;

extern device_state_t s_device_state;

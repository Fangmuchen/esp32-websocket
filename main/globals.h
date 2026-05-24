#pragma once

/*
 * globals.h — 全局配置和共享类型
 * ================================
 * 这个文件是整个项目的"公共头文件"，
 * 所有 .cpp 文件都会包含它（通过 #include "globals.h"）。
 * 这里放的是：
 *   1) 硬件引脚定义（LED 接哪个脚、按键接哪个脚）
 *   2) 配网参数（AP 名称前缀、超时时间）
 *   3) WebSocket 服务器地址
 *   4) NVS 存储的命名空间
 *   5) 设备状态枚举（当前处于什么工作模式）
 *   6) 默认 WiFi 凭据（编译时硬编码的备选）
 */

#include "driver/gpio.h"

// ========== 硬件配置 ==========
#define LED_PIN             GPIO_NUM_2   // WS2812 智能 LED 的数据引脚（GPIO2）
#define NUM_LEDS            1            // 灯珠数量（本例只用 1 颗）
#define BTN_CFG_PIN         GPIO_NUM_0   // BOOT 按键引脚（GPIO0），上电时长按 3 秒恢复出厂设置

// ========== I2C 配置（PCF8575 扩展 IO，控制 8 路灯光 + 8 路锁球器） ==========
#define I2C_SDA_PIN         GPIO_NUM_21  // I2C 数据引脚
#define I2C_SCL_PIN         GPIO_NUM_22  // I2C 时钟引脚

// ========== 配网配置 ==========
#define AP_SSID_PREFIX      "ESP32-LED"  // 热点名称前缀，实际名称 = 前缀 + MAC 地址后 3 字节
#define AP_CHANNEL          6            // WiFi 信道
#define WIFI_TIMEOUT_MS     20000        // 连接 WiFi 的最大等待时间（毫秒），超时则进入 AP 配网模式
#define AP_IDLE_TIMEOUT_US  300000000LL  // AP 模式空闲超时（微秒），300 秒无配网则自动重启

// ========== WebSocket 配置 ==========
#define WS_HOST             "app-y-259912-8-1434767743.sh.run.tcloudbase.com"  // 云服务器域名
#define WS_PORT             443                // 云服务器的 WebSocket 端口
#define WS_PATH             "/"                // WebSocket 路径

// ========== 软件版本 ==========
#define FW_VERSION          "2.0.0"            // 固件版本号

// ========== NTP 配置（用于获取正确时间，可选） ==========
// 如果启用 NTP，ESP32 会在 WiFi 连接后自动同步时间
// 同步成功后 datetime 字段才会有正确值
// #define ENABLE_NTP         1
// #define NTP_SERVER1         "pool.ntp.org"
// #define NTP_SERVER2         "time.nist.gov"
// #define NTP_TIMEZONE        "CST-8"

// ========== NVS（非易失性存储）配置 ==========
#define NVS_NAMESPACE       "ledctrl"    // NVS 中用于存储 WiFi 凭据的命名空间

// ========== WS2812 RMT（红外遥控模块）配置 ==========
#define RMT_RESOLUTION_HZ   10000000     // RMT 时钟频率 10MHz，用于生成 WS2812 的精确时序

// ========== Event Group Bits（事件组标志位） ==========
#define WIFI_CONNECTED_BIT  (1 << 0)     // WiFi 连接成功标志位
#define WIFI_FAIL_BIT       (1 << 1)     // WiFi 连接失败标志位

// ========== 默认 WiFi 凭据（当 NVS 中没有数据时使用） ==========
#define DEFAULT_SSID        "esp32"
#define DEFAULT_PASS        "123456789012"

// ========== 设备工作状态枚举 ==========
typedef enum {
    STATE_BOOT,            // 刚开机，正在初始化
    STATE_AP_CONFIG,       // 热点配网模式，等待用户手机连接并填写 WiFi 信息
    STATE_CONNECTING,      // 正在尝试连接 WiFi
    STATE_ONLINE           // WiFi 已连接，WebSocket 已连接，正常运行
} device_state_t;

extern device_state_t s_device_state;  // 全局设备状态变量（定义在 main.cpp）

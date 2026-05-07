#pragma once

/*
 * wifi_manager.h — WiFi 连接与热点模式（头文件）
 * ==============================================
 * 这个模块管理 ESP32 的所有 WiFi 操作：
 *   - connect_to_wifi() — 作为 STA（客户端）连接路由器
 *   - start_ap_mode()   — 作为 AP（热点）让手机连接配网
 *   - wifi_event_handler() — 处理 WiFi 连接/断开等事件
 *   - wifi_init_event_group() — 创建事件组，用于同步等待连接结果
 *
 * WiFi 有两种工作模式：
 *   1. STA 模式：ESP32 连接到你家的路由器
 *   2. AP 模式：ESP32 自己发射热点，手机连上来配网
 *   配网时我们用的是 APSTA 模式（同时做 AP 和 STA）
 */

#include <cstdint>
#include "esp_event.h"

extern char s_ap_ssid[32];  // AP 模式的热点名称（由 MAC 地址生成）

void wifi_init_event_group(void);
void wifi_event_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data);
bool connect_to_wifi(const char *ssid, const char *pass);
void start_ap_mode(void);

#pragma once

/*
 * websocket_client.h — WebSocket 客户端（头文件）v2.0
 * ===================================================
 * 优化要点：
 *   1. 精简协议：bitmask 整数替代 8 字符字符串
 *   2. 移除 token 校验（NTP 不可靠，token 始终为固定值）
 *   3. 新增断线自动重连
 *   4. 新增 WiFi 断开检测与重连
 */

#include "esp_websocket_client.h"

extern esp_websocket_client_handle_t s_ws_client;

void connect_websocket(void);
bool ws_is_connected(void);
void ws_tick(void);
void ws_set_heartbeat_interval(int seconds);
void ws_send_ping(void);

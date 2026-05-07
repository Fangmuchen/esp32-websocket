#pragma once

/*
 * websocket_client.h — WebSocket 客户端（头文件）
 * =================================================
 * 这个模块负责 ESP32 与云服务器之间的 WebSocket 通信。
 *
 * 工作流程：
 *   1) connect_websocket() — 连接到云服务器
 *   2) 连接成功后自动发送心跳包（包含设备状态）
 *   3) ws_tick() — 在主循环中定期调用，确保心跳按时发送
 *   4) 收到服务器的 "setting" 指令 → 解析并控制设备 → 回复结果
 *   5) ws_is_connected() — 检查 WebSocket 是否在线
 *
 * 协议文档参考 "无人台球馆硬件设备通讯文档.pdf"
 */

#include "esp_websocket_client.h"

extern esp_websocket_client_handle_t s_ws_client;  // WebSocket 客户端句柄

void connect_websocket(void);
bool ws_is_connected(void);
void ws_tick(void);
void ws_set_heartbeat_interval(int seconds);

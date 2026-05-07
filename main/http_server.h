#pragma once

/*
 * http_server.h — HTTP 配网服务器（头文件）
 * ==========================================
 * 这个模块提供配网用的 HTTP 服务。
 * 当 ESP32 处于 AP 配网模式时，手机浏览器访问 192.168.4.1 就能打开配网页面。
 *
 * 功能：
 *   1) 提供配网 HTML 页面（用户输入 WiFi 名称和密码）
 *   2) 提供 /api/status 接口（返回 AP 名称等信息）
 *   3) 接收 /api/config POST 请求（保存 WiFi 配置）
 *   4) 所有其他请求返回配网页面（确保不管用户怎么访问都能配网）
 *   5) 拒绝非 GET 请求（防止微信等 App 的探测连接占用连接数）
 */

#include "esp_http_server.h"

extern httpd_handle_t s_http_server;  // HTTP 服务器句柄

void start_http_server(void);

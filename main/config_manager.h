#pragma once

/*
 * config_manager.h — WiFi 配置存储（头文件）
 * ==========================================
 * 这个模块负责把 WiFi 的 SSID 和密码保存到 ESP32 的 NVS（非易失性存储）中。
 * NVS 是 ESP32 的"小硬盘"——掉电不丢失。
 *
 * 为什么需要存 WiFi？
 * 因为设备每次重启都要自动连接 WiFi，不能每次都让用户重新输入。
 * 第一次配网后，凭据就保存在 NVS 里了。
 *
 * 函数说明：
 *   ssid_is_valid(ssid) — 检查 SSID 格式是否合法（长度 ≤32，只含可打印字符）
 *   save_config(ssid, pass) — 将 WiFi 凭据写入 NVS
 *   load_config() — 从 NVS 读取 WiFi 凭据到 s_saved_ssid / s_saved_pass
 *   clear_config() — 擦除 NVS 中保存的 WiFi 凭据（恢复出厂）
 */

extern char s_saved_ssid[64];  // 全局变量：保存的 WiFi 名称
extern char s_saved_pass[64];  // 全局变量：保存的 WiFi 密码

bool ssid_is_valid(const char *ssid);
void save_config(const char *ssid, const char *pass);
bool load_config(void);
void clear_config(void);

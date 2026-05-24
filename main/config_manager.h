#pragma once

/*
 * config_manager.h — 配置存储（头文件）
 * =====================================
 * NVS（非易失性存储）管理：
 *   - WiFi 凭据（SSID + 密码）
 *   - 设备编号（SN）
 *   - 桌球台数量
 *   - 设备工作状态（灯光/锁/门/电源）
 */

#include <cstdint>

extern char s_saved_ssid[64];  // 全局变量：保存的 WiFi 名称
extern char s_saved_pass[64];  // 全局变量：保存的 WiFi 密码
extern char s_saved_sn[32];    // 全局变量：设备编号（默认 "FRH-001"，可由服务器修改）
extern int s_num_tables;       // 全局变量：桌球台数量（服务器可配置，无上限）

bool ssid_is_valid(const char *ssid);
void save_config(const char *ssid, const char *pass);
void save_sn(const char *sn);   // 写入 SN 到 NVS
void load_sn(void);             // 从 NVS 加载 SN
void save_num_tables(int n);   // 写入桌球台数量到 NVS
void load_num_tables(void);    // 从 NVS 加载桌球台数量
bool load_config(void);
void clear_config(void);

// 设备状态保存/恢复（每次 setting 命令后保存，启动时恢复）
// 状态字段：light_mask, lock_mask, audio_mask, door, pool_table
void save_device_state(uint8_t light, uint8_t lock, uint8_t audio, uint8_t door, uint8_t pt);
bool load_device_state(uint8_t *light, uint8_t *lock, uint8_t *audio, uint8_t *door, uint8_t *pt);

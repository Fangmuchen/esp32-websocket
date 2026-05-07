#pragma once

/*
 * device_control.h — 设备控制逻辑（头文件）
 * ==========================================
 * 这个模块管理所有"被控设备"的状态和操作。
 *
 * 本项目是"无人台球馆"的硬件控制器，控制三种设备：
 *   1) 灯光（8 路通道，编号 0-7）
 *      每路可以单独开关，对应台球桌上不同位置的灯
 *   2) 音响（8 路通道，但本项目只用第 0 路）
 *      用于语音播报（开台、续费提醒等）
 *   3) 台球桌电源开关（只有一路，开/关）
 *
 * control_process_command() 是核心函数，解析服务器下发的 JSON 命令，
 * 设置对应设备的状态，并返回执行结果。
 */

#include <cstdint>
#include "cJSON.h"

#define NUM_LIGHT_CHANNELS  8  // 灯光的通道数（8 路）
#define NUM_AUDIO_CHANNELS  8  // 音响的通道数（8 路，目前只用了第 0 路）

// 设备的当前状态
typedef struct {
    bool lights[NUM_LIGHT_CHANNELS];  // 每路灯的状态：true=开 false=关
    bool audio[NUM_AUDIO_CHANNELS];   // 每路音响的状态：true=开 false=关
    bool pool_table;                   // 台球桌电源：true=开 false=关
} control_state_t;

// 命令执行结果
typedef struct {
    bool processed;       // 命令是否被处理（是否匹配 "setting" 类型）
    bool interval_ok;     // 心跳间隔更新结果：true=成功 false=失败
    bool light_ok;        // 灯光控制结果：true=成功 false=失败
    bool audio_ok;        // 音响控制结果：true=成功 false=失败
    char remark[128];     // 失败原因描述（成功时为空字符串）
} command_result_t;

void control_init(void);
void control_set_light(int channel, bool on);
void control_set_audio(int channel, bool on);
void control_set_pool_table(bool on);
bool control_get_pool_table(void);
void control_get_light_str(char *buf, size_t len);  // 将 8 路灯状态拼接成字符串，如 "10101010"
void control_get_audio_str(char *buf, size_t len);  // 将 8 路音响状态拼接成字符串

// 解析 "setting" 命令，更新设备状态，返回结果码
command_result_t control_process_command(cJSON *root);

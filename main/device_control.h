#pragma once

/*
 * device_control.h — 设备控制逻辑（头文件）v2.0
 * =============================================
 * 优化：使用 uint8_t bitmask 代替 bool[8] 数组，更紧凑、更可靠。
 *
 * bitmask 格式：
 *   bit 0 = 通道 0, bit 7 = 通道 7
 *   例：0b01010101 (85) = 通道 0,2,4,6 开；通道 1,3,5,7 关
 */

#include <cstdint>
#include "cJSON.h"

#define NUM_CHANNELS        8   // 灯光/锁定/音频各 8 路
#define CONTROL_VERSION     "2.0"

// 设备当前状态（bitmask 表示）
typedef struct {
    uint8_t light_mask;    // 灯光：bit n = 通道 n (0=关, 1=开)
    uint8_t lock_mask;     // 锁定：同上
    uint8_t audio_mask;    // 音频：同上（仅 ch0 用于语音播报）
    uint8_t door;          // 门：0=关, 1=开
    uint8_t pool_table;    // 台球桌电源：0=关, 1=开
} control_state_t;

// 命令执行结果（按操作类型记录）
typedef struct {
    bool processed;        // 是否匹配 "setting" 命令
    bool interval_ok;      // 心跳间隔更新结果
    char result_json[128]; // 结果的 JSON 字符串：{"lt":0,"lk":0,"dr":0,"pt":0}
    char remark[64];       // 失败原因描述
} command_result_t;

void control_init(void);

// 单通道控制（用于 control_process_command 内部）
// 返回 true 表示硬件执行成功，false 表示硬件操作失败
bool control_set_light_ch(int channel, bool on);
bool control_set_lock_ch(int channel, bool on);
bool control_set_audio_ch(int channel, bool on);
bool control_set_door(bool open);
bool control_set_pool_table(bool on);

// 批量设置（用于场景控制）
bool control_set_all_lights(bool on);
bool control_set_all_locks(bool on);

// 读取状态
uint8_t control_get_light_mask(void);
uint8_t control_get_lock_mask(void);
uint8_t control_get_audio_mask(void);
uint8_t control_get_door(void);
uint8_t control_get_pool_table(void);
void    control_get_all_state(control_state_t *out);

// 解析 "setting" 命令，更新设备状态，返回结果
command_result_t control_process_command(cJSON *root);

// 将当前设备状态持久化到 NVS（每次 setting 命令后调用）
void control_persist_state(void);

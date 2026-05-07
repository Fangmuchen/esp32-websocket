#include "device_control.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "esp_log.h"

/*
 * device_control.cpp — 设备控制逻辑（实现）
 * ==========================================
 *
 * 这个模块维护设备的内部状态（control_state_t s_state），
 * 并提供两种接口：
 *   1) 底层接口：control_set_light() / control_set_audio() 等
 *      直接设置某一路的开关状态
 *   2) 高层接口：control_process_command()
 *      解析 JSON 命令，执行对应操作，返回结果
 *
 * control_process_command() 的输入示例：
 * {
 *   "type": "setting",
 *   "lightNo": 1,        // 第 2 路灯光（编号从 0 开始）
 *   "lightStatus": 1,    // 开灯
 *   "audioNo": 0,        // 第 1 路音响
 *   "audioStatus": 1,    // 开启
 *   "tableStatus": 0     // 关台球桌
 * }
 *
 * 字段验证规则：
 *   lightNo:     0-7（共 8 路）
 *   lightStatus: 0 或 1
 *   audioNo:     0 固定（本项目只有 1 路音响）
 *   audioStatus: 0 或 1
 *   tableStatus: 0 或 1
 */

static const char *TAG = "device";

static control_state_t s_state;  // 设备状态（全局静态变量，只能通过本模块的函数修改）

// ================================================================
//  初始化设备控制 — 将所有设备设为关闭状态
// ================================================================
void control_init(void)
{
    memset(&s_state, 0, sizeof(s_state));  // 全部置 0 = 关闭
    ESP_LOGI(TAG, "Control initialized (8 lights, 8 audio, pool table)");
}

// ================================================================
//  设置某一路灯的开关
//  channel: 0-7
//  on: true=开 false=关
//  只有在状态发生变化时才执行操作并打印日志
// ================================================================
void control_set_light(int channel, bool on)
{
    if (channel < 0 || channel >= NUM_LIGHT_CHANNELS) return;
    if (s_state.lights[channel] != on) {
        s_state.lights[channel] = on;
        ESP_LOGI(TAG, "Light %d: %s", channel + 1, on ? "ON" : "OFF");
        // TODO: 实际的 GPIO/继电器控制代码
    }
}

// ================================================================
//  设置某一路音响的开关
//  channel: 0-7（本项目只用了第 0 路）
// ================================================================
void control_set_audio(int channel, bool on)
{
    if (channel < 0 || channel >= NUM_AUDIO_CHANNELS) return;
    if (s_state.audio[channel] != on) {
        s_state.audio[channel] = on;
        ESP_LOGI(TAG, "Audio %d: %s", channel + 1, on ? "ON" : "OFF");
        // TODO: 实际的 GPIO/继电器控制代码
    }
}

// ================================================================
//  设置台球桌电源
// ================================================================
void control_set_pool_table(bool on)
{
    if (s_state.pool_table != on) {
        s_state.pool_table = on;
        ESP_LOGI(TAG, "Pool table: %s", on ? "ON" : "OFF");
        // TODO: 实际的 GPIO/继电器控制代码
    }
}

bool control_get_pool_table(void)
{
    return s_state.pool_table;
}

// ================================================================
//  获取 8 路灯的状态字符串
//  例如：所有灯都开着 → "11111111"
//        第 0、2 路开 → "10100000"
//  每位对应一路灯：1=开 0=关
// ================================================================
void control_get_light_str(char *buf, size_t len)
{
    if (len < (size_t)NUM_LIGHT_CHANNELS + 1) return;
    for (int i = 0; i < NUM_LIGHT_CHANNELS; i++)
        buf[i] = s_state.lights[i] ? '1' : '0';
    buf[NUM_LIGHT_CHANNELS] = '\0';
}

// ================================================================
//  获取 8 路音响的状态字符串
//  格式与灯光相同，每位对应一路音响
// ================================================================
void control_get_audio_str(char *buf, size_t len)
{
    if (len < (size_t)NUM_AUDIO_CHANNELS + 1) return;
    for (int i = 0; i < NUM_AUDIO_CHANNELS; i++)
        buf[i] = s_state.audio[i] ? '1' : '0';
    buf[NUM_AUDIO_CHANNELS] = '\0';
}

// ================================================================
//  处理服务器下发的 "setting" 命令（核心函数）
//  参数：cJSON 根节点（已由调用方解析好的 JSON 对象）
//  返回：command_result_t，包含每项的执行结果
//  失败时 result.remark 会填写原因说明
// ================================================================
command_result_t control_process_command(cJSON *root)
{
    command_result_t res;
    memset(&res, 0, sizeof(res));          // 初始全 0（所有字段默认 false/空）
    res.interval_ok = false;               // 没有 interval 参数也算"没有失败"
    res.light_ok = true;
    res.audio_ok = true;

    if (!root) {
        snprintf(res.remark, sizeof(res.remark), "null payload");
        return res;
    }

    cJSON *ti = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(ti)) return res;
    const char *type = ti->valuestring;

    // 只处理 type = "setting" 的命令
    if (strcmp(type, "setting") == 0) {
        res.processed = true;

        // —— 灯光控制：lightNo (0-7) + lightStatus (0/1) ——
        cJSON *ln = cJSON_GetObjectItem(root, "lightNo");
        cJSON *ls = cJSON_GetObjectItem(root, "lightStatus");
        if (cJSON_IsNumber(ln) && cJSON_IsNumber(ls)) {
            int ch = ln->valueint;
            // 验证：通道 0-7，状态只能 0 或 1
            if (ch >= 0 && ch < NUM_LIGHT_CHANNELS && (ls->valueint == 0 || ls->valueint == 1)) {
                control_set_light(ch, ls->valueint != 0);
                res.light_ok = true;
            } else {
                res.light_ok = false;
                snprintf(res.remark, sizeof(res.remark),
                         "lightNo=%d invalid (must be 0-%d), lightStatus=%d (must be 0/1)",
                         ch, NUM_LIGHT_CHANNELS - 1, ls->valueint);
                ESP_LOGW(TAG, "%s", res.remark);
            }
        } else if (cJSON_IsNumber(ln) || cJSON_IsNumber(ls)) {
            // 只有一个参数，另一个缺失
            res.light_ok = false;
            snprintf(res.remark, sizeof(res.remark),
                     "lightNo or lightStatus missing");
        }

        // —— 音响控制：audioNo (0=only) + audioStatus (0/1) ——
        cJSON *an = cJSON_GetObjectItem(root, "audioNo");
        cJSON *as = cJSON_GetObjectItem(root, "audioStatus");
        if (cJSON_IsNumber(an) && cJSON_IsNumber(as)) {
            // 验证：audioNo 必须为 0（只有一路），audioStatus 只能 0 或 1
            if (an->valueint == 0 && (as->valueint == 0 || as->valueint == 1)) {
                control_set_audio(0, as->valueint != 0);
                res.audio_ok = true;
            } else {
                res.audio_ok = false;
                snprintf(res.remark + strlen(res.remark), sizeof(res.remark) - strlen(res.remark),
                         "%saudioNo=%d invalid (must be 0), audioStatus=%d (must be 0/1)",
                         strlen(res.remark) > 0 ? "; " : "",
                         an->valueint, as->valueint);
                ESP_LOGW(TAG, "Invalid audio: No=%d, Status=%d", an->valueint, as->valueint);
            }
        } else if (cJSON_IsNumber(an) || cJSON_IsNumber(as)) {
            res.audio_ok = false;
            snprintf(res.remark + strlen(res.remark), sizeof(res.remark) - strlen(res.remark),
                     "%saudioNo or audioStatus missing",
                     strlen(res.remark) > 0 ? "; " : "");
        }

        // —— 台球桌控制：tableStatus (0/1) ——
        cJSON *ts = cJSON_GetObjectItem(root, "tableStatus");
        if (cJSON_IsNumber(ts)) {
            if (ts->valueint == 0 || ts->valueint == 1) {
                control_set_pool_table(ts->valueint != 0);
            } else {
                ESP_LOGW(TAG, "Invalid tableStatus: %d", ts->valueint);
            }
        }
    }

    return res;
}

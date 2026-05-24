#include "device_control.h"
#include "i2c_expander.h"
#include "config_manager.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "esp_log.h"
#include "driver/gpio.h"

/*
 * device_control.cpp — 设备控制逻辑（实现）v2.1
 * =============================================
 *
 * v2.1 变更（2026-05-23）：
 *   1. 灯光/锁球器改用 PCF8575 I2C 扩展 IO（低电平触发）
 *   2. 门/台球桌电源/蜂鸣器保留直连 GPIO
 *   3. 每次执行 setting 命令后自动保存状态到 NVS
 *   4. 启动时从 NVS 恢复上次状态
 */

static const char *TAG = "device";

static control_state_t s_state;

// ========== GPIO 引脚定义（PCB v2.1） ==========
// 灯光/锁球器 → PCF8575 #1 (0x20) / #2 (0x21)
#define GPIO_DOOR           GPIO_NUM_26   // 门锁继电器
#define GPIO_POOL_TABLE     GPIO_NUM_27   // 台球桌电源继电器
#define GPIO_AUDIO          GPIO_NUM_14   // 蜂鸣器/音频触发

static void init_gpio(void)
{
    // 门 + 台球桌 + 音频（PCF8575 不涉及这三个）
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_DOOR) | (1ULL << GPIO_POOL_TABLE) | (1ULL << GPIO_AUDIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(GPIO_DOOR, 0);
    gpio_set_level(GPIO_POOL_TABLE, 0);
    gpio_set_level(GPIO_AUDIO, 0);
}

// ========== 初始化 ==========
void control_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    init_gpio();
    expander_init();  // 初始化 PCF8575 ×2，全部输出高（继电器断开）

    // 从 NVS 恢复上次状态
    load_device_state(&s_state.light_mask, &s_state.lock_mask,
                      &s_state.audio_mask, &s_state.door, &s_state.pool_table);
    // 恢复灯光到 PCF8575
    for (int i = 0; i < NUM_CHANNELS; i++) {
        expander_write_light_pin(i, !((s_state.light_mask >> i) & 1));
    }
    // 恢复锁球器到 PCF8575
    for (int i = 0; i < NUM_CHANNELS; i++) {
        expander_write_locker_pin(i, !((s_state.lock_mask >> i) & 1));
    }
    // 恢复直连 GPIO
    gpio_set_level(GPIO_DOOR, s_state.door ? 1 : 0);
    gpio_set_level(GPIO_POOL_TABLE, s_state.pool_table ? 1 : 0);

    ESP_LOGI(TAG, "Device control v2.1 init: lt=0x%02X lk=0x%02X dr=%d pt=%d",
             s_state.light_mask, s_state.lock_mask, s_state.door, s_state.pool_table);
}

// ========== 持久化保存当前状态到 NVS ==========
void control_persist_state(void)
{
    save_device_state(s_state.light_mask, s_state.lock_mask,
                      s_state.audio_mask, s_state.door, s_state.pool_table);
}

// ========== 灯光（PCF8575 #2） ==========
bool control_set_light_ch(int channel, bool on)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    uint8_t mask = s_state.light_mask;
    uint8_t bit = 1 << channel;
    if (on) {
        if (!(mask & bit)) {
            esp_err_t err = expander_write_light_pin(channel, false);  // 低电平触发
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Light %d: I2C write failed (%s)", channel + 1, esp_err_to_name(err));
                return false;
            }
            mask |= bit;
            ESP_LOGI(TAG, "Light %d: ON", channel + 1);
        }
    } else {
        if (mask & bit) {
            esp_err_t err = expander_write_light_pin(channel, true);   // 高电平断开
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Light %d: I2C write failed (%s)", channel + 1, esp_err_to_name(err));
                return false;
            }
            mask &= ~bit;
            ESP_LOGI(TAG, "Light %d: OFF", channel + 1);
        }
    }
    s_state.light_mask = mask;
    return true;
}

// ========== 锁球器（PCF8575 #1） ==========
bool control_set_lock_ch(int channel, bool on)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    uint8_t mask = s_state.lock_mask;
    uint8_t bit = 1 << channel;
    if (on) {
        if (!(mask & bit)) {
            esp_err_t err = expander_write_locker_pin(channel, false);  // 低电平触发
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Lock %d: I2C write failed (%s)", channel + 1, esp_err_to_name(err));
                return false;
            }
            mask |= bit;
            ESP_LOGI(TAG, "Lock %d: ON", channel + 1);
        }
    } else {
        if (mask & bit) {
            esp_err_t err = expander_write_locker_pin(channel, true);   // 高电平断开
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Lock %d: I2C write failed (%s)", channel + 1, esp_err_to_name(err));
                return false;
            }
            mask &= ~bit;
            ESP_LOGI(TAG, "Lock %d: OFF", channel + 1);
        }
    }
    s_state.lock_mask = mask;
    return true;
}

// ========== 蜂鸣器/音频（直连 GPIO14） ==========
bool control_set_audio_ch(int channel, bool on)
{
    if (channel < 0 || channel >= NUM_CHANNELS) return false;
    uint8_t mask = s_state.audio_mask;
    uint8_t bit = 1 << channel;
    if (on) {
        if (!(mask & bit)) {
            mask |= bit;
            gpio_set_level(GPIO_AUDIO, 1);
            ESP_LOGI(TAG, "Audio %d: ON", channel + 1);
        }
    } else {
        if (mask & bit) {
            mask &= ~bit;
            gpio_set_level(GPIO_AUDIO, 0);
            ESP_LOGI(TAG, "Audio %d: OFF", channel + 1);
        }
    }
    s_state.audio_mask = mask;
    return true;
}

// ========== 门锁（直连 GPIO26） ==========
bool control_set_door(bool open)
{
    if (s_state.door != (uint8_t)open) {
        s_state.door = open ? 1 : 0;
        gpio_set_level(GPIO_DOOR, open ? 1 : 0);
        ESP_LOGI(TAG, "Door: %s", open ? "OPEN" : "CLOSE");
    }
    return true;
}

// ========== 台球桌电源（直连 GPIO27） ==========
bool control_set_pool_table(bool on)
{
    if (s_state.pool_table != (uint8_t)on) {
        s_state.pool_table = on ? 1 : 0;
        gpio_set_level(GPIO_POOL_TABLE, on ? 1 : 0);
        ESP_LOGI(TAG, "Pool table: %s", on ? "ON" : "OFF");
    }
    return true;
}

// ========== 批量控制 ==========
bool control_set_all_lights(bool on)
{
    uint8_t mask = on ? 0xFF : 0x00;
    if (s_state.light_mask != mask) {
        for (int i = 0; i < NUM_CHANNELS; i++) {
            esp_err_t err = expander_write_light_pin(i, !on);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "All lights: I2C write failed on ch%d (%s)", i, esp_err_to_name(err));
                return false;
            }
        }
        s_state.light_mask = mask;
        ESP_LOGI(TAG, "All lights: %s", on ? "ON" : "OFF");
    }
    return true;
}

bool control_set_all_locks(bool on)
{
    uint8_t mask = on ? 0xFF : 0x00;
    if (s_state.lock_mask != mask) {
        for (int i = 0; i < NUM_CHANNELS; i++) {
            esp_err_t err = expander_write_locker_pin(i, !on);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "All locks: I2C write failed on ch%d (%s)", i, esp_err_to_name(err));
                return false;
            }
        }
        s_state.lock_mask = mask;
        ESP_LOGI(TAG, "All locks: %s", on ? "ON" : "OFF");
    }
    return true;
}

// ========== 状态读取 ==========
uint8_t control_get_light_mask(void)  { return s_state.light_mask; }
uint8_t control_get_lock_mask(void)   { return s_state.lock_mask; }
uint8_t control_get_audio_mask(void)  { return s_state.audio_mask; }
uint8_t control_get_door(void)        { return s_state.door; }
uint8_t control_get_pool_table(void)  { return s_state.pool_table; }

void control_get_all_state(control_state_t *out)
{
    if (out) memcpy(out, &s_state, sizeof(s_state));
}

// ========== JSON 命令解析 ==========

static void append_result(command_result_t *res, const char *key, int ok)
{
    size_t len = strlen(res->result_json);
    if (len <= 2) {
        // "{}" → overwrite "}" with first key:value + "}"
        snprintf(res->result_json + 1, sizeof(res->result_json) - 1,
                 "\"%s\":%d}", key, ok ? 0 : 1);
    } else {
        // Already has items: insert before trailing "}"
        size_t pos = len - 1;  // position of closing "}"
        snprintf(res->result_json + pos, sizeof(res->result_json) - pos,
                 ",\"%s\":%d}", key, ok ? 0 : 1);
    }
}

/*
 * 提取 ch.<type> 对象中的 {"n": channel, "v": 0/1} 并执行
 * type_key: "lt" / "lk" / "au"
 * setter:   对应控制函数
 */
static void parse_channel_command(cJSON *ch_obj, const char *type_key,
                                   bool (*setter)(int, bool), command_result_t *res)
{
    cJSON *item = cJSON_GetObjectItem(ch_obj, type_key);
    if (!item) return; // 没指定这个类型的操作

    if (cJSON_IsObject(item)) {
        cJSON *n = cJSON_GetObjectItem(item, "n");
        cJSON *v = cJSON_GetObjectItem(item, "v");
        if (cJSON_IsNumber(n) && cJSON_IsNumber(v)) {
            int ch = n->valueint;
            int val = v->valueint;
            if (ch >= 0 && ch < NUM_CHANNELS && (val == 0 || val == 1)) {
                bool ok = setter(ch, val != 0);
                append_result(res, type_key, ok ? 1 : 0);  // 1=成功, 0=失败
            } else {
                append_result(res, type_key, 0);  // 0=失败
                snprintf(res->remark, sizeof(res->remark),
                         "%s: invalid n=%d v=%d", type_key, ch, val);
                ESP_LOGW(TAG, "%s", res->remark);
            }
        }
    }
}

// ========== 核心命令处理 ==========
command_result_t control_process_command(cJSON *root)
{
    command_result_t res;
    memset(&res, 0, sizeof(res));
    res.result_json[0] = '{';
    res.result_json[1] = '}';
    res.result_json[2] = '\0';
    res.interval_ok = true; // 修复：没有 interval 参数也算成功

    if (!root) {
        snprintf(res.remark, sizeof(res.remark), "null payload");
        return res;
    }

    cJSON *ti = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(ti)) return res;
    const char *type = ti->valuestring;

    if (strcmp(type, "setting") != 0) return res;
    res.processed = true;

    // —— 可选：更新心跳间隔 ——
    cJSON *iv = cJSON_GetObjectItem(root, "int");
    if (!cJSON_IsNumber(iv)) iv = cJSON_GetObjectItem(root, "interval");
    if (cJSON_IsNumber(iv)) {
        res.interval_ok = (iv->valueint >= 1);
    }

    // —— 解析 ch 对象（v2.0 格式：ch.lt = {"n":3, "v":1}） ——
    cJSON *ch = cJSON_GetObjectItem(root, "ch");
    if (cJSON_IsObject(ch)) {
        // 灯光控制：ch.lt = {"n":3, "v":1}
        parse_channel_command(ch, "lt", control_set_light_ch, &res);
        // 锁定控制：ch.lk
        parse_channel_command(ch, "lk", control_set_lock_ch, &res);
        // 音频控制：ch.au
        parse_channel_command(ch, "au", control_set_audio_ch, &res);
        // 门控制：ch.dr = 0/1
        cJSON *dr = cJSON_GetObjectItem(ch, "dr");
        if (cJSON_IsNumber(dr)) {
            if (dr->valueint == 0 || dr->valueint == 1) {
                control_set_door(dr->valueint != 0);
                append_result(&res, "dr", 1);
            } else {
                append_result(&res, "dr", 0);
                snprintf(res.remark, sizeof(res.remark), "dr: invalid value %d", dr->valueint);
            }
        }
        // 台球桌控制：ch.pt = 0/1
        cJSON *pt = cJSON_GetObjectItem(ch, "pt");
        if (cJSON_IsNumber(pt)) {
            if (pt->valueint == 0 || pt->valueint == 1) {
                control_set_pool_table(pt->valueint != 0);
                append_result(&res, "pt", 1);
            } else {
                append_result(&res, "pt", 0);
                snprintf(res.remark, sizeof(res.remark), "pt: invalid value %d", pt->valueint);
            }
        }
        // 批量灯光控制：ch.lt_all = 0/1
        cJSON *lt_all = cJSON_GetObjectItem(ch, "lt_all");
        if (cJSON_IsNumber(lt_all) && (lt_all->valueint == 0 || lt_all->valueint == 1)) {
            control_set_all_lights(lt_all->valueint != 0);
        }
        // 批量锁定控制：ch.lk_all = 0/1
        cJSON *lk_all = cJSON_GetObjectItem(ch, "lk_all");
        if (cJSON_IsNumber(lk_all) && (lk_all->valueint == 0 || lk_all->valueint == 1)) {
            control_set_all_locks(lk_all->valueint != 0);
        }
    }

    // —— 兼容服务器 flat 格式：lightNo/lightStatus, lockNo/lockStatus ——
    cJSON *light_no = cJSON_GetObjectItem(root, "lightNo");
    cJSON *light_st = cJSON_GetObjectItem(root, "lightStatus");
    if (cJSON_IsNumber(light_no) && cJSON_IsNumber(light_st)) {
        int ch_no = light_no->valueint;  // 服务器 0-based，直接映射通道
        if (ch_no >= 0 && ch_no < NUM_CHANNELS) {
            control_set_light_ch(ch_no, light_st->valueint != 0);
            append_result(&res, "lt", 1);
        }
    }

    cJSON *lock_no = cJSON_GetObjectItem(root, "lockNo");
    cJSON *lock_st = cJSON_GetObjectItem(root, "lockStatus");
    if (cJSON_IsNumber(lock_no) && cJSON_IsNumber(lock_st)) {
        int ch_no = lock_no->valueint;
        if (ch_no >= 0 && ch_no < NUM_CHANNELS) {
            control_set_lock_ch(ch_no, lock_st->valueint != 0);
            append_result(&res, "lk", 1);
        }
    }

    return res;
}

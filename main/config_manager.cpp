#include "globals.h"
#include "config_manager.h"
#include <cstring>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

/*
 * config_manager.cpp — WiFi 配置存储（实现）
 * ===========================================
 *
 * 使用 NVS 保存 WiFi 的 SSID 和密码。
 * 通过 HTTP 配网页面写入，设备重启后自动连接。
 */

static const char *TAG = "ledctrl";

char s_saved_ssid[64] = "";  // 空，需配网设置
char s_saved_pass[64] = "";
char s_saved_sn[32] = "FRH-001"; // 默认设备编号（桌球室名称）
int s_num_tables = 8;        // 默认 8 台，由服务器远程配置

// ================================================================
//  检查 SSID 格式是否合法
//  Wi-Fi 联盟规定 SSID 最大 32 字节，且应为可打印 ASCII 字符
//  返回 true = 合法，false = 非法
// ================================================================
bool ssid_is_valid(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') return false;          // 空指针或空字符串
    size_t len = strnlen(ssid, 33);
    if (len > 32) return false;                          // 超过 32 字节
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 0x20 || c > 0x7e) return false;          // 不可打印字符
    }
    return true;
}

// ================================================================
//  将 WiFi 凭据保存到 NVS
//  写入 ssid 和 pass 两个 key，然后调用 nvs_commit 确保写入完成
// ================================================================
void save_config(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed");
        return;
    }
    if (nvs_set_str(nvs, "ssid", ssid) != ESP_OK ||
        nvs_set_str(nvs, "pass", pass) != ESP_OK ||
        nvs_commit(nvs) != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed");
    } else {
        ESP_LOGI(TAG, "WiFi config saved (SSID=%s)", ssid);
    }
    nvs_close(nvs);
}

// ================================================================
//  从 NVS 加载 WiFi 凭据
//  返回 true = 可以继续（即使 NVS 没数据，也会用默认值）
//  如果 NVS 中的数据损坏/无效，会清空并用默认值
// ================================================================
bool load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return true;  // NVS 打不开（首次使用），用编译时的默认值

    // 读 SSID
    size_t len = sizeof(s_saved_ssid);
    esp_err_t err = nvs_get_str(nvs, "ssid", s_saved_ssid, &len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return true;  // NVS 中没数据，用默认值
    }
    // 读密码
    len = sizeof(s_saved_pass);
    err = nvs_get_str(nvs, "pass", s_saved_pass, &len);
    nvs_close(nvs);
    if (err != ESP_OK) s_saved_pass[0] = '\0';  // 没密码，置空

    // 如果 SSID 无效，清空
    if (s_saved_ssid[0] && !ssid_is_valid(s_saved_ssid)) {
        ESP_LOGW(TAG, "Stored SSID invalid, clearing");
        clear_config();
        s_saved_ssid[0] = '\0';
        s_saved_pass[0] = '\0';
    }

    if (s_saved_ssid[0]) {
        ESP_LOGI(TAG, "Using SSID=%s", s_saved_ssid);
    } else {
        ESP_LOGI(TAG, "No saved WiFi config");
    }
    return true;
}

// ================================================================
//  清除 NVS 中的 WiFi 配置（恢复出厂设置）
//  只删除 ssid 和 pass 两个 key，不影响 NVS 中的其他数据
// ================================================================
void clear_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return;
    nvs_erase_key(nvs, "ssid");
    nvs_erase_key(nvs, "pass");
    nvs_erase_key(nvs, "sn");
    nvs_erase_key(nvs, "num_tbl");
    nvs_erase_key(nvs, "dev_lt");
    nvs_erase_key(nvs, "dev_lk");
    nvs_erase_key(nvs, "dev_au");
    nvs_erase_key(nvs, "dev_dr");
    nvs_erase_key(nvs, "dev_pt");
    s_num_tables = 8;
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Config cleared");
}

// ================================================================
//  保存设备编号（SN）到 NVS
// ================================================================
void save_sn(const char *sn)
{
    if (!sn || sn[0] == '\0') return;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_str(nvs, "sn", sn) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
        strlcpy(s_saved_sn, sn, sizeof(s_saved_sn));
        ESP_LOGI(TAG, "SN saved: %s", s_saved_sn);
    }
    nvs_close(nvs);
}

// ================================================================
//  从 NVS 加载设备编号（SN）
//  如果 NVS 中没有保存，s_saved_sn 保持空字符串，
//  websocket_client 自动回退到 MAC 地址生成。
// ================================================================
void load_sn(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        // NVS 不可用，保留编译期默认值 "FRH-001"
        ESP_LOGI(TAG, "SN: %s (default)", s_saved_sn);
        return;
    }
    size_t len = sizeof(s_saved_sn);
    if (nvs_get_str(nvs, "sn", s_saved_sn, &len) != ESP_OK) {
        // NVS 中没有 SN，保留默认值 "FRH-001" 不清空
        ESP_LOGI(TAG, "SN: %s (default, no saved SN in NVS)", s_saved_sn);
        nvs_close(nvs);
        return;
    }
    nvs_close(nvs);

    ESP_LOGI(TAG, "SN: %s (from NVS)", s_saved_sn);
}

// ================================================================
//  保存桌球台数量到 NVS
//  服务器通过 {"type":"setting", "numTables":N} 远程配置
// ================================================================
void save_num_tables(int n)
{
    if (n < 1) return;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    int32_t val = (int32_t)n;
    if (nvs_set_i32(nvs, "num_tbl", val) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
        s_num_tables = n;
        ESP_LOGI(TAG, "numTables saved: %d", s_num_tables);
    }
    nvs_close(nvs);
}

// ================================================================
//  从 NVS 加载桌球台数量
//  未配置时默认 8 台（匹配硬件最大容量）
// ================================================================
void load_num_tables(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        s_num_tables = 8;
        return;
    }
    int32_t val = 0;
    if (nvs_get_i32(nvs, "num_tbl", &val) != ESP_OK || val < 1) {
        s_num_tables = 8;
    } else {
        s_num_tables = (int)val;
    }
    nvs_close(nvs);
    ESP_LOGI(TAG, "numTables: %d", s_num_tables);
}

// ========== 设备工作状态持久化 ==========

void save_device_state(uint8_t light, uint8_t lock, uint8_t audio, uint8_t door, uint8_t pt)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;

    bool ok = true;
    ok &= (nvs_set_u8(nvs, "dev_lt", light) == ESP_OK);
    ok &= (nvs_set_u8(nvs, "dev_lk", lock) == ESP_OK);
    ok &= (nvs_set_u8(nvs, "dev_au", audio) == ESP_OK);
    ok &= (nvs_set_u8(nvs, "dev_dr", door) == ESP_OK);
    ok &= (nvs_set_u8(nvs, "dev_pt", pt) == ESP_OK);
    ok &= (nvs_commit(nvs) == ESP_OK);

    if (ok) {
        ESP_LOGI(TAG, "Device state saved: lt=0x%02X lk=0x%02X dr=%d pt=%d",
                 light, lock, door, pt);
    } else {
        ESP_LOGW(TAG, "Device state save failed");
    }
    nvs_close(nvs);
}

bool load_device_state(uint8_t *light, uint8_t *lock, uint8_t *audio, uint8_t *door, uint8_t *pt)
{
    // 默认值：全关
    if (light) *light = 0;
    if (lock)  *lock  = 0;
    if (audio) *audio = 0;
    if (door)  *door  = 0;
    if (pt)    *pt    = 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "Device state: using defaults (all off)");
        return false;
    }

    if (light && nvs_get_u8(nvs, "dev_lt", light) != ESP_OK) *light = 0;
    if (lock  && nvs_get_u8(nvs, "dev_lk", lock)  != ESP_OK) *lock  = 0;
    if (audio && nvs_get_u8(nvs, "dev_au", audio) != ESP_OK) *audio = 0;
    if (door  && nvs_get_u8(nvs, "dev_dr", door)  != ESP_OK) *door  = 0;
    if (pt    && nvs_get_u8(nvs, "dev_pt", pt)    != ESP_OK) *pt    = 0;

    nvs_close(nvs);

    ESP_LOGI(TAG, "Device state loaded: lt=0x%02X lk=0x%02X dr=%d pt=%d",
             light ? *light : 0, lock ? *lock : 0, door ? *door : 0, pt ? *pt : 0);
    return true;
}

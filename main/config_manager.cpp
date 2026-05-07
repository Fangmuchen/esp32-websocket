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
 * 这个模块使用 ESP32 的 NVS（非易失性存储）库。
 * NVS 就像 ESP32 内置的一个小"文件系统"，以键值对方式存储数据。
 * 我们用它来保存 WiFi 的 SSID 和密码。
 *
 * 启动流程：
 *   上电 → load_config() 从 NVS 读 WiFi → 如果有数据就用，没有就用默认值
 *   配网 → save_config() 把用户输入的 WiFi 写入 NVS
 *   恢复出厂 → 按住 BOOT 键 3 秒 → clear_config() 擦除 NVS
 */

static const char *TAG = "ledctrl";

char s_saved_ssid[64] = DEFAULT_SSID;  // 保存的 WiFi 名称，默认 "esp32"
char s_saved_pass[64] = DEFAULT_PASS;  // 保存的 WiFi 密码，默认 "123456789012"

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

    // 如果 SSID 无效（比如被损坏了），清空并用默认值
    if (!ssid_is_valid(s_saved_ssid)) {
        ESP_LOGW(TAG, "Stored SSID invalid, using default");
        clear_config();
        strlcpy(s_saved_ssid, DEFAULT_SSID, sizeof(s_saved_ssid));
        strlcpy(s_saved_pass, DEFAULT_PASS, sizeof(s_saved_pass));
    }

    ESP_LOGI(TAG, "Using SSID=%s", s_saved_ssid);
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
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Config cleared");
}

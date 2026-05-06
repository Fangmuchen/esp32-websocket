#include "globals.h"
#include "config_manager.h"
#include <cstring>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "ledctrl";

char s_saved_ssid[64] = DEFAULT_SSID;
char s_saved_pass[64] = DEFAULT_PASS;

bool ssid_is_valid(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') return false;
    size_t len = strnlen(ssid, 33);
    if (len > 32) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

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

bool load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return true;

    size_t len = sizeof(s_saved_ssid);
    esp_err_t err = nvs_get_str(nvs, "ssid", s_saved_ssid, &len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return true;
    }
    len = sizeof(s_saved_pass);
    err = nvs_get_str(nvs, "pass", s_saved_pass, &len);
    nvs_close(nvs);
    if (err != ESP_OK) s_saved_pass[0] = '\0';

    if (!ssid_is_valid(s_saved_ssid)) {
        ESP_LOGW(TAG, "Stored SSID invalid, using default");
        clear_config();
        strlcpy(s_saved_ssid, DEFAULT_SSID, sizeof(s_saved_ssid));
        strlcpy(s_saved_pass, DEFAULT_PASS, sizeof(s_saved_pass));
    }

    ESP_LOGI(TAG, "Using SSID=%s", s_saved_ssid);
    return true;
}

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

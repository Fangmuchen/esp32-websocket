#include "globals.h"
#include "wifi_manager.h"
#include <cstring>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "ledctrl";

char s_ap_ssid[32];
static EventGroupHandle_t s_wifi_event_group = NULL;

void wifi_event_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        extern device_state_t s_device_state;
        if (s_device_state == STATE_CONNECTING)
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

EventGroupHandle_t wifi_get_event_group(void)
{
    return s_wifi_event_group;
}

void wifi_init_event_group(void)
{
    s_wifi_event_group = xEventGroupCreate();
}

bool connect_to_wifi(const char *ssid, const char *pass)
{
    extern device_state_t s_device_state;

    ESP_LOGI(TAG, "Connecting to %s ...", ssid);
    s_device_state = STATE_CONNECTING;

    wifi_config_t cfg = {};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdTRUE, pdFALSE, pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi OK");
        s_device_state = STATE_ONLINE;
        return true;
    }
    ESP_LOGW(TAG, "WiFi timeout");
    return false;
}

void start_ap_mode(void)
{
    extern device_state_t s_device_state;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X%02X",
             AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

    esp_netif_create_default_wifi_ap();

    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_ERROR_CHECK(stop_err);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = {};
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_device_state = STATE_AP_CONFIG;
    ESP_LOGI(TAG, "AP: %s (192.168.4.1)", s_ap_ssid);
}

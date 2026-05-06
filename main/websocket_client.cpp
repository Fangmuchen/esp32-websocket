#include "globals.h"
#include "websocket_client.h"
#include "led_control.h"
#include "device_control.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_rom_md5.h"

static const char *TAG = "ledctrl";

esp_websocket_client_handle_t s_ws_client = NULL;
#define HEARTBEAT_INTERVAL_DEFAULT  30

static int s_hb_interval = HEARTBEAT_INTERVAL_DEFAULT;

// ================================================================
//  Helpers
// ================================================================

static void get_sn(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "ESP32-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void get_datetime(char *buf, size_t len)
{
    snprintf(buf, len, "1970-01-01 00:00:00");
    time_t now = time(NULL);
    if (now > 0) {
        struct tm *tm = gmtime(&now);
        if (tm) strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm);
    }
}

// ================================================================
//  Token calculation (section 五 — token值说明)
//
//  token = md5(hex(md5(sn)) + "TQ" + datetime + type + hex(md5(data)))
//
//  Where md5() returns 16 binary bytes,
//        hex() converts to 32-char lowercase hex string,
//        data   = JSON string of business parameters (light, audio, interval, etc.)
// ================================================================

static void md5_hex(const char *input, char *output_hex)
{
    md5_context_t ctx;
    uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];

    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, input, strlen(input));
    esp_rom_md5_final(digest, &ctx);

    for (int i = 0; i < ESP_ROM_MD5_DIGEST_LEN; i++)
        sprintf(output_hex + i * 2, "%02x", digest[i]);
    output_hex[32] = '\0';
}

static void calculate_token(const char *sn, const char *datetime,
                             const char *type, const char *data_json,
                             char *token_out)
{
    char md5_sn[33], md5_data[33], combined[256];

    md5_hex(sn, md5_sn);
    md5_hex(data_json, md5_data);

    snprintf(combined, sizeof(combined), "%sTQ%s%s%s",
             md5_sn, datetime, type, md5_data);
    md5_hex(combined, token_out);
}

// ================================================================
//  Build business-data JSON string for token
// ================================================================

static char *build_setting_resp_data_json(command_result_t *res)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "interval", res ? (res->interval_ok ? 0 : 1) : 0);
    if (res) {
        cJSON_AddNumberToObject(d, "light", res->light_ok ? 0 : 1);
        cJSON_AddNumberToObject(d, "audio", res->audio_ok ? 0 : 1);
    }
    char *s = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    return s;
}

// ================================================================
//  Heartbeat — send ping packet (section 1.1)
// ================================================================

static void send_heartbeat_ping(void)
{
    if (!ws_is_connected()) return;

    char sn[24], datetime[24], token[33];
    char light_str[NUM_LIGHT_CHANNELS + 1];
    char audio_str[NUM_AUDIO_CHANNELS + 1];

    get_sn(sn, sizeof(sn));
    get_datetime(datetime, sizeof(datetime));
    control_get_light_str(light_str, sizeof(light_str));
    control_get_audio_str(audio_str, sizeof(audio_str));

    // Build token: data = business fields {"light":"...","audio":"...","interval":...}
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "light", light_str);
    cJSON_AddStringToObject(data, "audio", audio_str);
    cJSON_AddNumberToObject(data, "interval", s_hb_interval);
    char *data_json = cJSON_PrintUnformatted(data);
    calculate_token(sn, datetime, "ping", data_json, token);
    free(data_json);
    cJSON_Delete(data);

    cJSON *ping = cJSON_CreateObject();
    cJSON_AddStringToObject(ping, "type", "ping");
    cJSON_AddStringToObject(ping, "sn", sn);
    cJSON_AddNumberToObject(ping, "version", 1.2);
    cJSON_AddStringToObject(ping, "datetime", datetime);
    cJSON_AddStringToObject(ping, "token", token);
    cJSON_AddStringToObject(ping, "lightNo", light_str);
    cJSON_AddStringToObject(ping, "audioNo", audio_str);
    cJSON_AddNumberToObject(ping, "interval", s_hb_interval);

    char *s = cJSON_PrintUnformatted(ping);
    ESP_LOGI(TAG, "TX(heartbeat): lightNo=%s audioNo=%s interval=%d", light_str, audio_str, s_hb_interval);
    esp_websocket_client_send_text(s_ws_client, s, strlen(s), portMAX_DELAY);
    free(s);
    cJSON_Delete(ping);
}

void ws_tick(void)
{
    static int64_t last_hb = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_hb >= s_hb_interval * 1000000LL) {
        last_hb = now;
        send_heartbeat_ping();
    }
}

void ws_set_heartbeat_interval(int seconds)
{
    if (seconds < 1) seconds = 1;
    s_hb_interval = seconds;
    ESP_LOGI(TAG, "Heartbeat interval set to %ds", s_hb_interval);
}

// ================================================================
//  Send setting confirmation (section 2.2)
// ================================================================

static void send_setting_response(const char *logbn, command_result_t *res)
{
    if (!ws_is_connected()) return;

    char sn[24], datetime[24], token[33];
    get_sn(sn, sizeof(sn));
    get_datetime(datetime, sizeof(datetime));

    // Build token from response business data
    char *data_json = build_setting_resp_data_json(res);
    calculate_token(sn, datetime, "setting", data_json, token);
    free(data_json);

    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddStringToObject(rsp, "sn", sn);
    cJSON_AddNumberToObject(rsp, "version", 1.2);
    cJSON_AddStringToObject(rsp, "logbn", logbn ? logbn : "");
    cJSON_AddStringToObject(rsp, "type", "setting");
    cJSON_AddStringToObject(rsp, "datetime", datetime);
    cJSON_AddStringToObject(rsp, "token", token);

    if (res) {
        cJSON_AddNumberToObject(rsp, "interval", res->interval_ok ? 0 : 1);
        cJSON_AddNumberToObject(rsp, "light", res->light_ok ? 0 : 1);
        cJSON_AddNumberToObject(rsp, "audio", res->audio_ok ? 0 : 1);
    } else {
        cJSON_AddNumberToObject(rsp, "interval", 0);
    }

    char *s = cJSON_PrintUnformatted(rsp);
    esp_websocket_client_send_text(s_ws_client, s, strlen(s), portMAX_DELAY);
    free(s);
    cJSON_Delete(rsp);
}

// ================================================================
//  WebSocket Event Handler
// ================================================================

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    esp_websocket_client_handle_t cli = (esp_websocket_client_handle_t)arg;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS connected — sending initial ping");
        send_heartbeat_ping();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WS disconnected");
        break;

    case WEBSOCKET_EVENT_DATA: {
        esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
        if (ev->op_code != 1) break;  // text frames only

        // Copy payload
        char *payload = (char *)malloc(ev->data_len + 1);
        if (!payload) break;
        memcpy(payload, ev->data_ptr, ev->data_len);
        payload[ev->data_len] = '\0';

        // —— Serial print: every received message ——
        ESP_LOGI(TAG, "RX(%u): %s", ev->data_len, payload);

        // Parse and handle
        cJSON *root = cJSON_Parse(payload);
        if (root) {
            cJSON *ti = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(ti)) {
                const char *type = ti->valuestring;

                if (strcmp(type, "setting") == 0) {
                    // Extract logbn for response echo
                    cJSON *lb = cJSON_GetObjectItem(root, "logbn");
                    const char *logbn = cJSON_IsString(lb) ? lb->valuestring : NULL;

                    // Update interval if provided
                    cJSON *iv = cJSON_GetObjectItem(root, "interval");
                    if (cJSON_IsNumber(iv) && iv->valueint > 0) {
                        s_hb_interval = iv->valueint;
                        ESP_LOGI(TAG, "Heartbeat interval -> %ds", s_hb_interval);
                    }

                    // Process command and send confirmation (section 2.2)
                    command_result_t cres = control_process_command(root);
                    send_setting_response(logbn, &cres);

                    // Immediately send heartbeat with updated state
                    send_heartbeat_ping();

                } else if (strcmp(type, "pong") == 0 || strcmp(type, "ping") == 0) {
                    // Platform response — already logged via RX above
                } else if (strcmp(type, "welcome") == 0) {
                    ESP_LOGI(TAG, "Server welcome");
                } else {
                    ESP_LOGI(TAG, "Unknown type: %s", type);
                }
            }
            cJSON_Delete(root);
        }
        free(payload);
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WS error");
        break;
    default:
        break;
    }
}

// ================================================================
//  Public API
// ================================================================

void connect_websocket(void)
{
    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%d%s", WS_HOST, WS_PORT, WS_PATH);

    esp_websocket_client_config_t cfg = { .uri = uri };
    s_ws_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, s_ws_client);
    esp_websocket_client_start(s_ws_client);
    ESP_LOGI(TAG, "WS client starting -> %s", uri);
}

bool ws_is_connected(void)
{
    return s_ws_client && esp_websocket_client_is_connected(s_ws_client);
}

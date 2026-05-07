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

/*
 * websocket_client.cpp — WebSocket 客户端（实现）
 * ================================================
 *
 * 这个文件实现了"无人台球馆"协议的 WebSocket 客户端部分。
 * 协议分成以下几个部分：
 *
 * 1.1 心跳包（ping）— 设备 → 服务器
 *     定时发送，包含设备序列号、时间、所有灯和音响的状态。
 *     token 用于验证数据合法性。
 *
 * 1.2 平台返回（pong）— 服务器 → 设备
 *     服务器对心跳的回应，用 ESP_LOGI 打印到串口方便调试。
 *
 * 2.1 反向控制（setting）— 服务器 → 设备
 *     平台下发控制指令，控制指定的灯、音响、台球桌开关。
 *
 * 2.2 设备返回确认 — 设备 → 服务器
 *     设备执行指令后，返回执行结果（成功/失败 + 失败原因）。
 *
 * 5. token 计算规则
 *     token = md5(hex(md5(sn)) + "TQ" + datetime + type + hex(md5(data)))
 *     这是协议的安全校验机制。
 */

static const char *TAG = "ledctrl";

esp_websocket_client_handle_t s_ws_client = NULL;
#define HEARTBEAT_INTERVAL_DEFAULT  30  // 默认心跳间隔 30 秒

static int s_hb_interval = HEARTBEAT_INTERVAL_DEFAULT;

// ================================================================
//  工具函数：获取设备序列号（基于 MAC 地址）
//  格式：ESP32-XXXXXXXXXXXX（MAC 地址的 12 位十六进制）
// ================================================================
static void get_sn(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "ESP32-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ================================================================
//  工具函数：获取当前时间字符串
//  如果 NTP 还没同步到时间，会返回 "1970-01-01 00:00:00"
// ================================================================
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
//  Token 计算（协议第五部分）
//
//  公式：
//    token = md5(hex(md5(sn)) + "TQ" + datetime + type + hex(md5(data)))
//
//  其中：
//    md5()     = 16 字节二进制摘要
//    hex()     = 32 字符小写十六进制字符串
//    "TQ"      = 固定拼接字符串
//    data      = 业务参数的 JSON 字符串（如灯、音响的状态）
//
//  md5_hex() 函数：对输入做 MD5，再转成 32 字符的十六进制字符串
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

    md5_hex(sn, md5_sn);           // hex(md5(sn))
    md5_hex(data_json, md5_data);  // hex(md5(data))

    snprintf(combined, sizeof(combined), "%sTQ%s%s%s",
             md5_sn, datetime, type, md5_data);
    md5_hex(combined, token_out);  // md5(上面的拼接结果)
}

// ================================================================
//  构造 setting 响应的业务数据 JSON（用于计算 token）
//  数据包含 interval / light / audio 的结果码，以及失败时的 remark
// ================================================================
static char *build_setting_resp_data_json(command_result_t *res)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "interval", res ? (res->interval_ok ? 0 : 1) : 0);
    if (res) {
        cJSON_AddNumberToObject(d, "light", res->light_ok ? 0 : 1);
        cJSON_AddNumberToObject(d, "audio", res->audio_ok ? 0 : 1);
        if (res->remark[0]) {
            cJSON_AddStringToObject(d, "remark", res->remark);
        }
    }
    char *s = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    return s;
}

// ================================================================
//  发送心跳包（协议 1.1）
//  包含：序列号、版本、时间、token、所有灯的状态、所有音响的状态、心跳间隔
//  这个函数在以下两种情况下被调用：
//    1) WebSocket 刚连接成功时（ws_event_handler 中）
//    2) 定时调用（ws_tick 中，每隔 s_hb_interval 秒）
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

    // 计算 token
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "light", light_str);
    cJSON_AddStringToObject(data, "audio", audio_str);
    cJSON_AddNumberToObject(data, "interval", s_hb_interval);
    char *data_json = cJSON_PrintUnformatted(data);
    calculate_token(sn, datetime, "ping", data_json, token);
    free(data_json);
    cJSON_Delete(data);

    // 构造 JSON 包
    cJSON *ping = cJSON_CreateObject();
    cJSON_AddStringToObject(ping, "type", "ping");
    cJSON_AddStringToObject(ping, "sn", sn);
    cJSON_AddNumberToObject(ping, "version", 1.2);
    cJSON_AddStringToObject(ping, "datetime", datetime);
    cJSON_AddStringToObject(ping, "token", token);
    cJSON_AddStringToObject(ping, "lightNo", light_str);  // 8 位字符串，每 1 位代表 1 路灯光
    cJSON_AddStringToObject(ping, "audioNo", audio_str);  // 8 位字符串，每 1 位代表 1 路音响
    cJSON_AddNumberToObject(ping, "interval", s_hb_interval);

    char *s = cJSON_PrintUnformatted(ping);
    ESP_LOGI(TAG, "TX(heartbeat): lightNo=%s audioNo=%s interval=%d", light_str, audio_str, s_hb_interval);
    esp_websocket_client_send_text(s_ws_client, s, strlen(s), portMAX_DELAY);
    free(s);
    cJSON_Delete(ping);
}

// ================================================================
//  心跳定时器（在 STATE_ONLINE 主循环中每次迭代调用）
//  用 esp_timer_get_time() 获取微秒级时间，判断是否到了发送时刻
// ================================================================
void ws_tick(void)
{
    static int64_t last_hb = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_hb >= s_hb_interval * 1000000LL) {
        last_hb = now;
        send_heartbeat_ping();
    }
}

// ================================================================
//  设置心跳间隔（服务器通过 setting 命令的 interval 字段修改）
//  最小间隔为 1 秒
// ================================================================
void ws_set_heartbeat_interval(int seconds)
{
    if (seconds < 1) seconds = 1;
    s_hb_interval = seconds;
    ESP_LOGI(TAG, "Heartbeat interval set to %ds", s_hb_interval);
}

// ================================================================
//  发送 setting 响应（协议 2.2）
//  设备执行完服务器的控制命令后，回复执行结果：
//    interval: 0=成功 1=失败
//    light:    0=成功 1=失败
//    audio:    0=成功 1=失败
//    remark:   失败时的原因描述（成功时不包含此字段）
// ================================================================

static void send_setting_response(const char *logbn, command_result_t *res)
{
    if (!ws_is_connected()) return;

    char sn[24], datetime[24], token[33];
    get_sn(sn, sizeof(sn));
    get_datetime(datetime, sizeof(datetime));

    // 构造业务数据并计算 token
    char *data_json = build_setting_resp_data_json(res);
    calculate_token(sn, datetime, "setting", data_json, token);
    free(data_json);

    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddStringToObject(rsp, "sn", sn);
    cJSON_AddNumberToObject(rsp, "version", 1.2);
    cJSON_AddStringToObject(rsp, "logbn", logbn ? logbn : "");  // 与下发指令的 logbn 一致
    cJSON_AddStringToObject(rsp, "type", "setting");
    cJSON_AddStringToObject(rsp, "datetime", datetime);
    cJSON_AddStringToObject(rsp, "token", token);

    if (res) {
        cJSON_AddNumberToObject(rsp, "interval", res->interval_ok ? 0 : 1);
        cJSON_AddNumberToObject(rsp, "light", res->light_ok ? 0 : 1);
        cJSON_AddNumberToObject(rsp, "audio", res->audio_ok ? 0 : 1);
        if (res->remark[0]) {
            cJSON_AddStringToObject(rsp, "remark", res->remark);  // 失败原因
        }
    } else {
        cJSON_AddNumberToObject(rsp, "interval", 0);
    }

    char *s = cJSON_PrintUnformatted(rsp);
    esp_websocket_client_send_text(s_ws_client, s, strlen(s), portMAX_DELAY);
    free(s);
    cJSON_Delete(rsp);
}

// ================================================================
//  WebSocket 事件处理函数
//  这是整个协议的核心入口，所有接收到的消息都在这里处理：
//
//  WEBSOCKET_EVENT_CONNECTED — 连接成功，立即发送第一个心跳包
//  WEBSOCKET_EVENT_DATA     — 收到数据，根据 type 字段分发处理
//    - "setting"  → 解析并执行控制命令，回复执行结果，再发心跳更新状态
//    - "pong"     → 服务器对心跳的回应，仅打印日志
//    - "welcome"  → 服务器欢迎消息
//  WEBSOCKET_EVENT_DISCONNECTED — 断开连接，打印日志
//  WEBSOCKET_EVENT_ERROR        — 出错，打印日志
// ================================================================

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg;  // 未使用（使用全局 s_ws_client 而不是 arg）

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS connected — sending initial ping");
        send_heartbeat_ping();  // 连接成功立即发一次心跳
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WS disconnected");
        break;

    case WEBSOCKET_EVENT_DATA: {
        esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
        if (ev->op_code != 1) break;  // 只处理文本帧（op_code=1），二进制帧跳过

        // 复制消息内容到独立缓冲区（ev->data_ptr 可能在回调外失效）
        char *payload = (char *)malloc(ev->data_len + 1);
        if (!payload) break;
        memcpy(payload, ev->data_ptr, ev->data_len);
        payload[ev->data_len] = '\0';

        // —— 串口打印：所有收到的消息（方便调试） ——
        ESP_LOGI(TAG, "RX(%u): %s", ev->data_len, payload);

        // 解析 JSON 并处理
        cJSON *root = cJSON_Parse(payload);
        if (root) {
            cJSON *ti = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(ti)) {
                const char *type = ti->valuestring;

                if (strcmp(type, "setting") == 0) {
                    // —— 收到控制命令（协议 2.1） ——
                    cJSON *lb = cJSON_GetObjectItem(root, "logbn");
                    const char *logbn = cJSON_IsString(lb) ? lb->valuestring : NULL;

                    // 更新心跳间隔（如果服务器指定了新间隔）
                    cJSON *iv = cJSON_GetObjectItem(root, "interval");
                    if (cJSON_IsNumber(iv) && iv->valueint > 0) {
                        s_hb_interval = iv->valueint;
                        ESP_LOGI(TAG, "Heartbeat interval -> %ds", s_hb_interval);
                    }

                    // 处理控制命令并发送响应（协议 2.2）
                    command_result_t cres = control_process_command(root);
                    send_setting_response(logbn, &cres);

                    // 立即发送心跳包，让服务器知道设备的最新状态
                    send_heartbeat_ping();

                } else if (strcmp(type, "pong") == 0 || strcmp(type, "ping") == 0) {
                    // 服务器的回应消息 — 已在 RX 日志中打印
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
//  连接 WebSocket 服务器
//  使用 esp_websocket_client 组件，配置服务器地址并启动
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

// ================================================================
//  检查 WebSocket 是否已连接
// ================================================================
bool ws_is_connected(void)
{
    return s_ws_client && esp_websocket_client_is_connected(s_ws_client);
}

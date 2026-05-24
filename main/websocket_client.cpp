#include "globals.h"
#include "websocket_client.h"
#include "led_control.h"
#include "device_control.h"
#include "config_manager.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

/*
 * websocket_client.cpp — WebSocket 客户端（实现）v2.0
 * ===================================================
 *
 * 协议 v2.0（优化版）：
 *
 * 1. 心跳（设备 → 服务器）
 *    {"type":"ping","sn":"ESP32-XXX","v":"2.0","lt":85,"lk":0,"au":0,"dr":0,"pt":0}
 *    lt/lk/au = uint8_t bitmask (0-255), dr/pt = 0/1
 *
 * 2. 控制命令（服务器 → 设备）
 *    {"type":"setting","id":"cmd_xxx","ch":{"lt":{"n":3,"v":1},"dr":1}}
 *    ch.lt = {n:通道(0-7), v:值(0/1)}
 *    ch.lk / ch.au 同上
 *    ch.dr / ch.pt = 直接 0/1
 *
 * 3. 控制回复（设备 → 服务器）
 *    {"type":"setting","sn":"ESP32-XXX","id":"cmd_xxx","r":{"lt":0,"dr":0}}
 *    之后立即发送一次心跳同步最新状态
 *
 * 优化说明：
 *   - 移除 token（NTP 未初始化导致 datetime 始终为 1970-01-01，token 完全可预测）
 *   - 用 uint8_t bitmask 替代 8 字符字符串，JSON 传输更紧凑
 *   - 添加自动重连机制
 */

static const char *TAG = "ledctrl";

esp_websocket_client_handle_t s_ws_client = NULL;
#define HEARTBEAT_INTERVAL_DEFAULT  30

static int s_hb_interval = HEARTBEAT_INTERVAL_DEFAULT;
static bool s_reconnect_pending = false;

// ========== 工具函数：获取设备序列号 ==========
static void get_sn(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "ESP32-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ========== 发送心跳包（v2.0 精简格式） ==========
static void send_heartbeat_ping(void)
{
    if (!ws_is_connected()) return;

    char sn[24];
    get_sn(sn, sizeof(sn));

    cJSON *ping = cJSON_CreateObject();
    cJSON_AddStringToObject(ping, "type", "ping");
    cJSON_AddStringToObject(ping, "sn", sn);
    cJSON_AddStringToObject(ping, "name", s_saved_sn);
    cJSON_AddStringToObject(ping, "v", CONTROL_VERSION);
    cJSON_AddNumberToObject(ping, "numTables", s_num_tables);
    cJSON_AddNumberToObject(ping, "lt", control_get_light_mask());
    cJSON_AddNumberToObject(ping, "lk", control_get_lock_mask());
    cJSON_AddNumberToObject(ping, "au", control_get_audio_mask());
    cJSON_AddNumberToObject(ping, "dr", control_get_door());
    cJSON_AddNumberToObject(ping, "pt", control_get_pool_table());

    char *s = cJSON_PrintUnformatted(ping);
    ESP_LOGI(TAG, "TX ping: lt=%d lk=%d dr=%d pt=%d",
             control_get_light_mask(), control_get_lock_mask(),
             control_get_door(), control_get_pool_table());
    esp_websocket_client_send_text(s_ws_client, s, strlen(s), portMAX_DELAY);
    free(s);
    cJSON_Delete(ping);
}

// ========== 发送 setting 响应（v2.0） ==========
static void send_setting_response(const char *req_id, command_result_t *res)
{
    if (!ws_is_connected()) return;

    char sn[24];
    get_sn(sn, sizeof(sn));

    cJSON *rsp = cJSON_CreateObject();
    cJSON_AddStringToObject(rsp, "type", "setting");
    cJSON_AddStringToObject(rsp, "sn", sn);
    if (req_id) {
        cJSON_AddStringToObject(rsp, "id", req_id);
    }

    // result 对象：记录每条命令的执行结果（0=成功, 非0=失败）
    if (res && strlen(res->result_json) > 2) {
        cJSON *parsed = cJSON_Parse(res->result_json);
        if (parsed) {
            cJSON_AddItemToObject(rsp, "r", parsed);
        } else {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "int", res->interval_ok ? 0 : 1);
            cJSON_AddItemToObject(rsp, "r", r);
        }
    } else {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "int", res ? (res->interval_ok ? 0 : 1) : 0);
        cJSON_AddItemToObject(rsp, "r", r);
    }

    // 附带完整设备状态（服务器用这些字段更新设备记录）
    cJSON_AddNumberToObject(rsp, "lt", control_get_light_mask());
    cJSON_AddNumberToObject(rsp, "lk", control_get_lock_mask());
    cJSON_AddNumberToObject(rsp, "au", control_get_audio_mask());
    cJSON_AddNumberToObject(rsp, "dr", control_get_door());
    cJSON_AddNumberToObject(rsp, "pt", control_get_pool_table());

    // 如果 remark 有内容，附带
    if (res && res->remark[0]) {
        cJSON_AddStringToObject(rsp, "remark", res->remark);
    }

    char *s = cJSON_PrintUnformatted(rsp);
    ESP_LOGI(TAG, "TX setting rsp: %s", s);
    esp_websocket_client_send_text(s_ws_client, s, strlen(s), portMAX_DELAY);
    free(s);
    cJSON_Delete(rsp);
}

// ========== 心跳定时器 ==========
void ws_tick(void)
{
    // 如果有重连请求且未连接，尝试重连
    if (s_reconnect_pending && !ws_is_connected()) {
        if (s_ws_client) {
            ESP_LOGI(TAG, "Attempting reconnect...");
            esp_websocket_client_start(s_ws_client);
            s_reconnect_pending = false;
        }
    }

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

void ws_send_ping(void)
{
    send_heartbeat_ping();
}

// ========== WebSocket 事件处理 ==========
static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS connected");
        s_reconnect_pending = false;
        // 连接成功立即发送心跳
        send_heartbeat_ping();
        // 重置心跳定时器，避免立即发第二次
        {
            static int64_t last_hb = 0;
            last_hb = esp_timer_get_time();
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WS disconnected — will reconnect on next tick");
        s_reconnect_pending = true;
        break;

    case WEBSOCKET_EVENT_DATA: {
        esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
        if (ev->op_code != 1) break; // text only

        char *payload = (char *)malloc(ev->data_len + 1);
        if (!payload) break;
        memcpy(payload, ev->data_ptr, ev->data_len);
        payload[ev->data_len] = '\0';

        ESP_LOGI(TAG, "RX(%u)", ev->data_len);
        printf("%s\n", payload);

        cJSON *root = cJSON_Parse(payload);
        if (root) {
            cJSON *ti = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(ti)) {
                const char *type = ti->valuestring;

                if (strcmp(type, "setting") == 0) {
                    // —— 控制命令 ——
                    cJSON *id_item = cJSON_GetObjectItem(root, "id");
                    const char *req_id = cJSON_IsString(id_item) ? id_item->valuestring : NULL;

                    // 更新心跳间隔（int / interval）
                    cJSON *iv = cJSON_GetObjectItem(root, "int");
                    if (!cJSON_IsNumber(iv)) iv = cJSON_GetObjectItem(root, "interval");
                    if (cJSON_IsNumber(iv) && iv->valueint >= 1) {
                        s_hb_interval = iv->valueint;
                    }

                    // 处理命令
                    command_result_t cres = control_process_command(root);
                    control_persist_state();
                    send_setting_response(req_id, &cres);

                    // 立即发送心跳同步最新状态
                    send_heartbeat_ping();

                } else if (strcmp(type, "pong") == 0) {
                    // 服务器回应 — 已在 RX 日志中显示
                } else if (strcmp(type, "session_warning") == 0) {
                    // —— 到期预警 ——
                    cJSON *tn = cJSON_GetObjectItem(root, "tableNo");
                    int table = cJSON_IsNumber(tn) ? tn->valueint : -1;
                    ESP_LOGI(TAG, "Session warning for table %d", table);
                    // 蜂鸣 2 声（脉冲音频引脚）
                    control_set_audio_ch(0, 1);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    control_set_audio_ch(0, 0);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    control_set_audio_ch(0, 1);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    control_set_audio_ch(0, 0);
                    // WS2812 橙色快闪
                    set_led_color(255, 165, 0);
                    blink_led(3, 150);
                    // 回复 ack
                    char sn[24];
                    get_sn(sn, sizeof(sn));
                    cJSON *ack = cJSON_CreateObject();
                    cJSON_AddStringToObject(ack, "type", "session_warning");
                    cJSON_AddStringToObject(ack, "sn", sn);
                    cJSON_AddStringToObject(ack, "result", "ok");
                    char *s = cJSON_PrintUnformatted(ack);
                    esp_websocket_client_send_text(s_ws_client, s, strlen(s), portMAX_DELAY);
                    free(s);
                    cJSON_Delete(ack);
                } else if (strcmp(type, "session_expired") == 0) {
                    // —— 到期关灯锁球 ——
                    cJSON *tn = cJSON_GetObjectItem(root, "tableNo");
                    if (cJSON_IsNumber(tn)) {
                        int table = tn->valueint;
                        ESP_LOGI(TAG, "Session expired for table %d", table);
                        control_set_light_ch(table, false);
                        control_set_lock_ch(table, true);
                    } else {
                        ESP_LOGI(TAG, "Session expired for all tables");
                        control_set_all_lights(false);
                        control_set_all_locks(true);
                        control_set_door(false);
                    }
                    // 蜂鸣 3 声
                    for (int i = 0; i < 3; i++) {
                        control_set_audio_ch(0, 1);
                        vTaskDelay(pdMS_TO_TICKS(200));
                        control_set_audio_ch(0, 0);
                        vTaskDelay(pdMS_TO_TICKS(300));
                    }
                    // WS2812 红色
                    set_led_color(255, 0, 0);
                    // 保存状态（灯/锁/门已变更）
                    control_persist_state();
                } else if (strcmp(type, "voice") == 0) {
                    // —— 语音播报 ——
                    cJSON *fi = cJSON_GetObjectItem(root, "file");
                    int file_no = cJSON_IsNumber(fi) ? fi->valueint : 0;
                    ESP_LOGI(TAG, "Voice play file %d", file_no);
                    // 脉冲音频引脚触发 DFPlayer Mini
                    control_set_audio_ch(0, 1);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    control_set_audio_ch(0, 0);
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
        s_reconnect_pending = true;
        break;

    default:
        break;
    }
}

// ========== 连接 WebSocket 服务器 ==========
void connect_websocket(void)
{
    char uri[128];
    snprintf(uri, sizeof(uri), "wss://%s:%d%s", WS_HOST, WS_PORT, WS_PATH);

    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
    };
    s_ws_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, s_ws_client);
    esp_websocket_client_start(s_ws_client);
    ESP_LOGI(TAG, "WS client -> %s", uri);
}

bool ws_is_connected(void)
{
    return s_ws_client && esp_websocket_client_is_connected(s_ws_client);
}

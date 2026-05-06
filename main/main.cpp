#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_websocket_client.h"
#include "esp_check.h"

// DNS (captive portal)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "ledctrl";

// ========== 硬件配置 ==========
#define LED_PIN          GPIO_NUM_2  // WS2812 数据脚
#define NUM_LEDS         1           // 灯珠数量
#define BTN_CFG_PIN      GPIO_NUM_0  // BOOT按钮=GPIO0, 上电时长按3秒清除配网

// ========== 配网配置 ==========
#define AP_SSID_PREFIX   "ESP32-LED"
#define AP_CHANNEL       6
#define WIFI_TIMEOUT_MS  20000     // 连接WiFi超时(ms)
#define AP_IDLE_TIMEOUT  300000    // AP模式5分钟无配网则重启

// ========== WebSocket 配置 ==========
#define WS_HOST          "your-server.com"
#define WS_PORT          8080
#define WS_PATH          "/"

// ========== NVS ==========
#define NVS_NAMESPACE    "ledctrl"

// ========== WS2812 RMT ==========
#define RMT_RESOLUTION_HZ 10000000 // 10MHz

// ========== Event Group ==========
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// ========== 状态枚举 ==========
typedef enum {
    STATE_BOOT,
    STATE_AP_CONFIG,
    STATE_CONNECTING,
    STATE_ONLINE
} device_state_t;

// ========== 全局变量 ==========
static device_state_t s_device_state = STATE_BOOT;
static EventGroupHandle_t s_wifi_event_group;
static esp_websocket_client_handle_t s_ws_client = NULL;
static httpd_handle_t s_http_server = NULL;
static char s_ap_ssid[32];
static bool s_led_state = false;
static int64_t s_ap_start_us = 0;

static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;

static char s_saved_ssid[64] = "";
static char s_saved_pass[64] = "";

// ================================================================
//  WS2812 LED 控制 (RMT)
// ================================================================

static esp_err_t led_init(void)
{
    rmt_tx_channel_config_t tx_chan = {
        .gpio_num = LED_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_chan, &s_led_chan), TAG, "create RMT TX ch");

    rmt_bytes_encoder_config_t enc_cfg = {};
    enc_cfg.bit0.duration0 = 4;
    enc_cfg.bit0.level0 = 1;
    enc_cfg.bit0.duration1 = 8;
    enc_cfg.bit0.level1 = 0;
    enc_cfg.bit1.duration0 = 7;
    enc_cfg.bit1.level0 = 1;
    enc_cfg.bit1.duration1 = 6;
    enc_cfg.bit1.level1 = 0;
    enc_cfg.flags.msb_first = 1;
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &s_led_encoder), TAG, "create bytes enc");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_chan), TAG, "enable RMT ch");
    return ESP_OK;
}

static void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    // WS2812 默认 GRB 顺序
    uint8_t data[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s_led_chan, s_led_encoder, data, sizeof(data), &tx_cfg);
    rmt_tx_wait_all_done(s_led_chan, portMAX_DELAY);
}

static void set_led(bool on)
{
    s_led_state = on;
    if (on)
        ws2812_set_color(255, 255, 255); // 白
    else
        ws2812_set_color(0, 0, 0);
    ESP_LOGI(TAG, "LED %s", on ? "ON (white)" : "OFF");
}

static void set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_led_state = (r | g | b) != 0;
    ws2812_set_color(r, g, b);
    ESP_LOGI(TAG, "LED color: R=%d G=%d B=%d", r, g, b);
}

static void blink_led(int count, int delay_ms)
{
    for (int i = 0; i < count; i++) {
        ws2812_set_color(255, 255, 255);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        ws2812_set_color(0, 0, 0);
        if (i < count - 1)
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ================================================================
//  NVS 配置读写
// ================================================================

static void save_config(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
        return;
    nvs_set_str(nvs, "ssid", ssid);
    nvs_set_str(nvs, "pass", pass);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "WiFi config saved");
}

static bool load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;

    size_t len = sizeof(s_saved_ssid);
    if (nvs_get_str(nvs, "ssid", s_saved_ssid, &len) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    len = sizeof(s_saved_pass);
    if (nvs_get_str(nvs, "pass", s_saved_pass, &len) != ESP_OK) {
        nvs_close(nvs);
        return false;
    }
    nvs_close(nvs);
    ESP_LOGI(TAG, "Loaded SSID=%s", s_saved_ssid);
    return true;
}

static void clear_config(void)
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

// ================================================================
//  WiFi 事件处理
// ================================================================

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_device_state == STATE_CONNECTING)
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ================================================================
//  WiFi 连接
// ================================================================

static bool connect_to_wifi(const char *ssid, const char *pass)
{
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

static void start_ap_mode(void)
{
    // AP 名从 MAC 生成
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X%02X",
             AP_SSID_PREFIX, mac[3], mac[4], mac[5]);

    // 创建 AP netif（STA netif 已在 main_task 创建）
    esp_netif_create_default_wifi_ap();

    // 停 WiFi（首次开机时 WiFi 可能未启动，忽略 NOT_STARTED 错误）
    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_ERROR_CHECK(stop_err);
    }
    // APSTA 模式：AP 供手机连接 + STA 供扫描用（纯 AP 下 scan 不可用）
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = {};
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    // 等电源稳定再初始化 PHY（减少 brownout 概率）
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_device_state = STATE_AP_CONFIG;
    s_ap_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "AP: %s (192.168.4.1)", s_ap_ssid);
}

// ================================================================
//  WiFi 扫描（HTTP handler 中同步调用）
// ================================================================

static cJSON *scan_networks(void)
{
    wifi_scan_config_t sc = {};
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 120;
    sc.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
        return cJSON_CreateArray();
    }

    uint16_t cnt = 0;
    err = esp_wifi_scan_get_ap_num(&cnt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan get_ap_num failed: %s", esp_err_to_name(err));
        return cJSON_CreateArray();
    }

    ESP_LOGI(TAG, "scan found %d APs", cnt);

    cJSON *arr = cJSON_CreateArray();
    if (cnt == 0) return arr;

    wifi_ap_record_t *rec = (wifi_ap_record_t *)calloc(cnt, sizeof(*rec));
    if (!rec) return arr;

    esp_wifi_scan_get_ap_records(&cnt, rec);
    for (uint16_t i = 0; i < cnt; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", (char *)rec[i].ssid);
        cJSON_AddNumberToObject(n, "rssi", rec[i].rssi);
        cJSON_AddBoolToObject(n, "enc", rec[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, n);
    }
    free(rec);
    return arr;
}

// ================================================================
//  DNS Server (Captive Portal) — 将所有域名解析到 192.168.4.1
// ================================================================

static void dns_server_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS: socket failed");
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "DNS: bind port 53 failed (already in use?)");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS server ready (captive portal)");

    uint8_t buf[512];
    uint8_t resp[512];

    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                          (struct sockaddr *)&from, &fromlen);
        if (len < 12) continue;

        // 复制查询作为响应基础
        memcpy(resp, buf, len);

        // 设置 DNS 响应标志: QR=1, AA=1, RA=1
        resp[2] = 0x85 | (buf[2] & 0x01);  // QR=1, AA=1, 保留 RD
        resp[3] = 0x80;                     // RA=1

        // 找到问题域名的结尾 (QNAME)
        int qend = 12;
        while (qend < len && buf[qend] != 0) {
            if ((buf[qend] & 0xC0) == 0xC0) { qend += 2; break; }
            qend += buf[qend] + 1;
        }
        if (qend >= len) continue;
        qend += 1; // 跳过结尾 0x00
        if (qend + 4 > len) continue;

        // 只响应 A 记录查询
        uint16_t qtype = (buf[qend] << 8) | buf[qend + 1];
        if (qtype == 1) {
            uint8_t *ans = resp + qend + 4;
            ans[0] = 0xC0; ans[1] = 0x0C;    // Name 指针指向问题
            ans[2] = 0x00; ans[3] = 0x01;    // Type A
            ans[4] = 0x00; ans[5] = 0x01;    // Class IN
            ans[6] = 0x00; ans[7] = 0x00;    // TTL = 60s
            ans[8] = 0x00; ans[9] = 0x3C;
            ans[10] = 0x00; ans[11] = 0x04;  // RDLENGTH = 4
            ans[12] = 192; ans[13] = 168;    // IP 192.168.4.1
            ans[14] = 4;   ans[15] = 1;
            resp[6] = 0x00; resp[7] = 0x01;  // ANCOUNT = 1
            sendto(sock, resp, qend + 4 + 16, 0,
                  (struct sockaddr *)&from, fromlen);
        } else {
            // 非 A 查询, 无答案
            resp[7] = 0x00;
            sendto(sock, resp, len, 0,
                  (struct sockaddr *)&from, fromlen);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

static void start_dns_server(void)
{
    xTaskCreate(dns_server_task, "dns", 3072, NULL, 3, NULL);
}

// ================================================================
//  HTTP Server
// ================================================================

// ================================================================
//  Captive Portal 页面 — 浏览器弹出配网界面
// ================================================================

// HTML 前半部分（不含 JS，不含 AP 名称——AP 名由 JS 从 /api/status 获取）
static const char s_html_pre[] =
    "<!DOCTYPE html><html lang='zh-CN'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 配网</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "background:#f5f5f7;color:#1d1d1f;padding:20px;max-width:420px;margin:0 auto}"
    "h1{font-size:22px;font-weight:600;text-align:center;margin:18px 0 4px}"
    ".sub{text-align:center;font-size:13px;color:#86868b;margin-bottom:20px}"
    ".card{background:#fff;border-radius:12px;padding:16px;margin-bottom:12px;"
    "box-shadow:0 1px 3px rgba(0,0,0,.08)}"
    ".ssid-item{display:flex;align-items:center;padding:10px 8px;"
    "border-bottom:1px solid #e8e8ed;cursor:pointer}"
    ".ssid-item:last-child{border-bottom:none}"
    ".ssid-item.selected{background:#e8f0fe;border-radius:8px;margin:2px 0}"
    ".ssid-item .name{flex:1;font-size:15px;font-weight:500;overflow:hidden;"
    "text-overflow:ellipsis;white-space:nowrap}"
    ".ssid-item .rssi{font-size:12px;color:#86868b;margin-left:8px}"
    ".lock-icon{margin-right:8px}"
    ".form-group{margin-top:16px}"
    ".form-group label{font-size:14px;font-weight:500;margin-bottom:6px;display:block}"
    ".form-group input{width:100%;padding:10px 12px;border:1px solid #d2d2d7;"
    "border-radius:8px;font-size:15px;outline:none;box-sizing:border-box}"
    ".form-group input:focus{border-color:#007aff}"
    ".btn{width:100%;padding:12px;background:#007aff;color:#fff;border:none;"
    "border-radius:8px;font-size:16px;font-weight:500;cursor:pointer;margin-top:14px}"
    ".btn:disabled{opacity:.4;cursor:not-allowed}"
    ".msg{padding:10px;border-radius:8px;margin:0 0 12px;font-size:14px;text-align:center}"
    ".msg.ok{background:#e8f5e9;color:#2e7d32}"
    ".msg.err{background:#fce4ec;color:#c62828}"
    "#list{max-height:320px;overflow-y:auto}"
    ".sp{display:inline-block;width:14px;height:14px;border:2px solid #e8e8ed;"
    "border-top-color:#007aff;border-radius:50%;animation:s .6s linear infinite;"
    "vertical-align:middle;margin-right:4px}"
    "@keyframes s{to{transform:rotate(360deg)}}"
    "</style></head><body>"
    "<h1>ESP32 LED 配网</h1>"
    "<p class='sub' id='apname'></p><div id='msg'></div>"
    "<div class='card'>"
    "<div style='display:flex;justify-content:space-between;"
    "align-items:center;margin-bottom:8px'>"
    "<strong>WiFi 网络</strong>"
    "<button onclick='scan()' style='background:none;border:none;color:#007aff;"
    "font-size:14px;cursor:pointer'>刷新</button></div>"
    "<div id='list'><p style='color:#86868b;text-align:center;padding:20px' "
    "id='scanstatus'><span class=sp></span>正在扫描...</p></div></div>"
    "<div class='card' id='cfgcard' style='display:none'>"
    "<div class='form-group'><label id='selabel'></label>"
    "<input type='password' id='pwd' placeholder='WiFi 密码（留空无密码）'>"
    "</div><button class='btn' id='connbtn' onclick='conn()'>连接 WiFi</button></div>"
;

// HTML 后半部分（JS 逻辑，无 ES6 语法兼容旧手机浏览器）
static const char s_html_post[] =
    "<script>"
    "var sel='',sc=false;"
    "function m(t,c){document.getElementById('msg').innerHTML="
    "'<div class=msg '+c+'>'+t+'</div>';"
    "if(c!='err')setTimeout(function(){document.getElementById('msg').innerHTML=''},5000)}"
    "function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;')"
    ".replace(/>/g,'&gt;').replace(/\"/g,'&quot;')}"
    "async function scan(){if(sc)return;sc=true;"
    "document.getElementById('scanstatus').innerHTML="
    "'<span class=sp></span>正在扫描...';"
    "var l=document.getElementById('list');"
    "var ac=new AbortController();"
    "setTimeout(function(){ac.abort()},8000);"
    "try{var r=await fetch('/api/scan',{signal:ac.signal});"
    "var n=await r.json();"
    "if(!n||n.length===0){l.innerHTML="
    "'<p style=color:#86868b;text-align:center;padding:20px>未扫描到WiFi网络</p>';return}"
    "n.sort(function(a,b){return b.rssi-a.rssi});"
    "var u=[],h={};n.forEach(function(x){if(!h[x.ssid]){h[x.ssid]=1;u.push(x)}});"
    "l.innerHTML=u.map(function(x){var lk=x.enc?'&#x1F512;':'&#x1F310;';"
    "var ba=x.rssi>-50?'&#x2581;&#x2581;&#x2581;&#x2581;':"
    "x.rssi>-65?'&#x2581;&#x2581;&#x2581;':"
    "x.rssi>-80?'&#x2581;&#x2581;':'&#x2581;';"
    "return '<div class=ssid-item data-ssid=\\\"'+esc(x.ssid)+'\\\" onclick=sel(this)>'"
    "+lk+'<span class=name>'+esc(x.ssid)+'</span>"
    "+'<span class=rssi>'+ba+' '+x.rssi+'dBm</span></div>'}).join('')"
    "}catch(e){l.innerHTML="
    "'<p style=color:#c62828;text-align:center;padding:20px>扫描失败或超时，请刷新重试</p>'}"
    "sc=false}"
    "function sel(el){document.querySelectorAll('.ssid-item').forEach("
    "function(e){e.classList.remove('selected')});el.classList.add('selected');"
    "sel=el.dataset.ssid;"
    "document.getElementById('selabel').textContent='\\u5df2\\u9009: '+sel;"
    "document.getElementById('cfgcard').style.display='block';"
    "setTimeout(function(){document.getElementById('pwd').focus()},300)}"
    "async function conn(){var s=sel;if(!s)return m('\\u8bf7\\u5148\\u9009\\u62e9WiFi\\u7f51\\u7edc','err');"
    "var p=document.getElementById('pwd').value;"
    "var b=document.getElementById('connbtn');b.disabled=true;b.textContent='\\u8fde\\u63a5\\u4e2d...';"
    "try{await fetch('/api/config',{method:'POST',"
    "headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:s,password:p})});"
    "m('\\u914d\\u7f6e\\u5df2\\u4fdd\\u5b58\\uff0c\\u8bbe\\u5907\\u6b63\\u5728\\u91cd\\u542f...','ok')"
    "}catch(e){m('\\u53d1\\u9001\\u5931\\u8d25\\uff0c\\u8bf7\\u91cd\\u8bd5','err')"
    ";b.disabled=false;b.textContent='\\u8fde\\u63a5 WiFi'}}"
    "fetch('/api/status').then(function(r){return r.json()})"
    ".then(function(d){document.getElementById('apname').textContent='AP: '+d.ap})"
    ".catch(function(){document.getElementById('apname').textContent='AP: ESP32-LED'});"
    "scan();"
    "</script></body></html>";

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, s_html_pre);
    httpd_resp_sendstr_chunk(req, s_html_post);
    httpd_resp_sendstr_chunk(req, "");
    return ESP_OK;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "status", "waiting");
    cJSON_AddStringToObject(doc, "ap", s_ap_ssid);
    char *json = cJSON_PrintUnformatted(doc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(doc);
    return ESP_OK;
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *doc = cJSON_Parse(buf);
    if (!doc) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *si = cJSON_GetObjectItem(doc, "ssid");
    if (!cJSON_IsString(si)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        cJSON_Delete(doc);
        return ESP_FAIL;
    }
    const char *ssid = si->valuestring;
    cJSON *pi = cJSON_GetObjectItem(doc, "password");
    const char *pass = cJSON_IsString(pi) ? pi->valuestring : "";
    cJSON_Delete(doc);

    ESP_LOGI(TAG, "Config: SSID=%s", ssid);
    save_config(ssid, pass);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"rebooting\":true}");

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    cJSON *arr = scan_networks();
    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t handle_404(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, s_html_pre);
    httpd_resp_sendstr_chunk(req, s_html_post);
    httpd_resp_sendstr_chunk(req, "");
    return ESP_OK;
}

// 处理 CONNECT 请求（微信 mmtls 隧道探测等），尽快关闭避免耗尽连接池
static esp_err_t handle_connect(httpd_req_t *req)
{
    ESP_LOGD(TAG, "CONNECT %s → 405 close", req->uri);
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "");
    return ESP_FAIL;  // 强制关闭连接
}

static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 10;          // 默认 7，微信 mmtls 占用多需扩容
    cfg.backlog_conn = 7;               // 默认 5
    if (httpd_start(&s_http_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    httpd_uri_t hu;
    hu = (httpd_uri_t){ .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    hu = (httpd_uri_t){ .uri = "/api/status", .method = HTTP_GET, .handler = handle_status, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    hu = (httpd_uri_t){ .uri = "/api/config", .method = HTTP_POST, .handler = handle_config_post, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    hu = (httpd_uri_t){ .uri = "/api/scan", .method = HTTP_GET, .handler = handle_scan, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    hu = (httpd_uri_t){ .uri = "/*", .method = HTTP_GET, .handler = handle_404, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    // CONNECT 方法（微信 mmtls/HTTP 方法3）→ 立即关闭释放 socket，防止耗尽连接池
    hu = (httpd_uri_t){ .uri = "/*", .method = (httpd_method_t)3, .handler = handle_connect, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);

    // 启动 DNS 服务器（捕获门户）
    start_dns_server();

    ESP_LOGI(TAG, "HTTP server on port 80 + DNS captive portal");
}

// ================================================================
//  WebSocket 客户端
// ================================================================

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    esp_websocket_client_handle_t cli = (esp_websocket_client_handle_t)arg;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "WS connected");
        // 发送 hello
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "hello");
        cJSON_AddStringToObject(msg, "device", "esp32-led");
        char ip[16] = "0.0.0.0";
        esp_netif_ip_info_t ipi;
        esp_netif_t *ni = esp_netif_get_handle_from_ifkey("STA_DEF");
        if (ni && esp_netif_get_ip_info(ni, &ipi) == ESP_OK)
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ipi.ip));
        cJSON_AddStringToObject(msg, "ip", ip);
        char *s = cJSON_PrintUnformatted(msg);
        esp_websocket_client_send_text(cli, s, strlen(s), portMAX_DELAY);
        free(s);
        cJSON_Delete(msg);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WS disconnected");
        break;

    case WEBSOCKET_EVENT_DATA: {
        esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
        if (ev->op_code != 1) break; // 非文本帧忽略

        char *payload = (char *)malloc(ev->data_len + 1);
        if (!payload) break;
        memcpy(payload, ev->data_ptr, ev->data_len);
        payload[ev->data_len] = '\0';

        // 尝试 JSON 解析
        cJSON *root = cJSON_Parse(payload);
        if (root) {
            cJSON *ti = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(ti) && strcmp(ti->valuestring, "state") == 0) {
                int rv = 255, gv = 255, bv = 255;
                bool onv = true;
                cJSON *ri = cJSON_GetObjectItem(root, "r");
                if (ri) rv = ri->valueint;
                cJSON *gi = cJSON_GetObjectItem(root, "g");
                if (gi) gv = gi->valueint;
                cJSON *bi = cJSON_GetObjectItem(root, "b");
                if (bi) bv = bi->valueint;
                cJSON *oni = cJSON_GetObjectItem(root, "on");
                if (oni) onv = cJSON_IsTrue(oni);
                if (onv) set_led_color(rv, gv, bv);
                else set_led(false);
            }
            cJSON_Delete(root);
        } else {
            // 纯文本兼容
            if      (strcmp(payload, "ON") == 0)     set_led(true);
            else if (strcmp(payload, "OFF") == 0)    set_led(false);
            else if (strcmp(payload, "TOGGLE") == 0) set_led(!s_led_state);
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

static void connect_websocket(void)
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
//  主任务（状态机）
// ================================================================

static void main_task(void *arg)
{
    // ---- 启动提示 ----
    blink_led(2, 200);

    // ---- 检测 BOOT 按键：按住 3 秒恢复出厂 ----
    gpio_set_direction(BTN_CFG_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_CFG_PIN, GPIO_PULLUP_ONLY);

    if (gpio_get_level(BTN_CFG_PIN) == 0) {
        ESP_LOGI(TAG, "BOOT pressed, hold 3s for reset...");
        bool do_reset = true;
        for (int i = 0; i < 30; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(BTN_CFG_PIN) != 0) { do_reset = false; break; }
        }
        if (do_reset) {
            ESP_LOGI(TAG, "Factory reset!");
            clear_config();
            blink_led(6, 150);
        }
    }

    // ---- 创建 WiFi netif ----
    esp_netif_create_default_wifi_sta();

    // ---- 尝试加载配置并连接 WiFi ----
    if (load_config()) {
        if (connect_to_wifi(s_saved_ssid, s_saved_pass)) {
            connect_websocket();
            goto state_loop;
        }
        // 连接失败 → 进入 AP 模式
    }

    // ---- AP 配网模式 ----
    ESP_LOGI(TAG, "Enter AP config mode");
    blink_led(5, 300);
    start_ap_mode();
    start_http_server();

state_loop:
    while (1) {
        switch (s_device_state) {

        case STATE_AP_CONFIG: {
            // AP 空闲超时
            if ((esp_timer_get_time() - s_ap_start_us) > (int64_t)AP_IDLE_TIMEOUT * 1000) {
                ESP_LOGI(TAG, "AP idle timeout, reboot");
                esp_restart();
            }
            // LED 慢闪
            static int64_t last_blink = 0;
            int64_t now = esp_timer_get_time();
            if (now - last_blink > 1000000) {
                static int phase = 0;
                ws2812_set_color(0, 0, (++phase % 2) ? 50 : 0);
                last_blink = now;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }

        case STATE_ONLINE: {
            // 定时汇报状态（30s）
            static int64_t last_report = 0;
            int64_t now = esp_timer_get_time();
            if (now - last_report > 30000000) {
                last_report = now;
                if (s_ws_client && esp_websocket_client_is_connected(s_ws_client)) {
                    cJSON *msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(msg, "type", "state");
                    cJSON_AddStringToObject(msg, "led", s_led_state ? "on" : "off");
                    cJSON_AddStringToObject(msg, "mode", "ws2812");
                    char *s = cJSON_PrintUnformatted(msg);
                    esp_websocket_client_send_text(s_ws_client, s, strlen(s), portMAX_DELAY);
                    free(s);
                    cJSON_Delete(msg);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }

        default:
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

// ================================================================
//  app_main — ESP-IDF 入口
// ================================================================

extern "C" void app_main(void)
{
    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 网络、事件循环
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // WiFi 初始化（暂不启动）
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    // 注册事件
    s_wifi_event_group = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               wifi_event_handler, NULL);

    // WS2812 LED 初始化
    ESP_ERROR_CHECK(led_init());
    set_led(false);

    // 主任务
    xTaskCreate(main_task, "main", 4096, NULL, 5, NULL);
}

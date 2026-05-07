#include "globals.h"
#include "http_server.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "dns_server.h"
#include <cstring>
#include <cstdlib>
#include "esp_log.h"
#include "cJSON.h"

/*
 * http_server.cpp — HTTP 配网服务器（实现）
 * ==========================================
 *
 * 这个模块是"配网体验"的核心。
 * 用户手机连上 ESP32 的热点后，通过 DNS 劫持 + HTTP 服务器，
 * 无论用户想访问什么网站，都会被引导到这个配网页面。
 *
 * 【配网流程】
 * 手机连 ESP32 热点 → 打开任意网址 → DNS 劫持到 192.168.4.1
 * → HTTP 服务器返回配网页面 → 用户填写 WiFi 名称密码
 * → 点击"连接" → POST /api/config → 保存到 NVS → 设备重启
 * → 重启后连接 WiFi → WebSocket 连接云服务器 → 正常使用
 */

static const char *TAG = "ledctrl";

httpd_handle_t s_http_server = NULL;  // HTTP 服务器句柄

// ================================================================
//  HTML 配网页面（前半部分）
//  包含 CSS 样式和页面结构
// ================================================================
static const char s_html_pre[] =
    "<!DOCTYPE html><html lang='zh-CN'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 配网</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "background:#f5f5f7;color:#1d1d1f;padding:20px;max-width:400px;margin:0 auto}"
    "h1{font-size:22px;font-weight:600;text-align:center;margin:18px 0 4px}"
    ".sub{text-align:center;font-size:13px;color:#86868b;margin-bottom:20px}"
    ".card{background:#fff;border-radius:12px;padding:16px 16px 8px;margin-bottom:12px;"
    "box-shadow:0 1px 3px rgba(0,0,0,.08)}"
    ".fg{margin-bottom:14px}"
    ".fg label{font-size:14px;font-weight:500;display:block;margin-bottom:5px}"
    ".fg input{width:100%;padding:10px 12px;border:1px solid #d2d2d7;"
    "border-radius:8px;font-size:15px;outline:none;box-sizing:border-box}"
    ".fg input:focus{border-color:#007aff}"
    ".btn{width:100%;padding:12px;background:#007aff;color:#fff;border:none;"
    "border-radius:8px;font-size:16px;font-weight:500;cursor:pointer;margin-top:4px}"
    ".btn:disabled{opacity:.4;cursor:not-allowed}"
    ".msg{padding:10px;border-radius:8px;margin:0 0 12px;font-size:14px;text-align:center}"
    ".msg.ok{background:#e8f5e9;color:#2e7d32}"
    ".msg.err{background:#fce4ec;color:#c62828}"
    "</style></head><body>"
    "<h1>ESP32 LED 配网</h1>"
    "<p class='sub' id='apname'></p><div id='msg'></div>"
    "<div class='card'>"
    "<div class='fg'><label>WiFi 名称</label>"
    "<input type='text' id='ssid' placeholder='请填写手机当前连接的WiFi名称'>"
    "</div><div class='fg'><label>WiFi 密码</label>"
    "<input type='password' id='pwd' placeholder='留空表示无密码'>"
    "</div><button class='btn' id='connbtn' onclick='conn()'>连接 WiFi</button></div>"
;

// ================================================================
//  HTML 配网页面（后半部分 — JavaScript）
//  1) 页面加载时请求 /api/status 获取 AP 名称
//  2) 用户点击"连接 WiFi" → POST /api/config 提交凭据
//  3) 提交成功显示"配置已保存，设备正在重启"
// ================================================================
static const char s_html_post[] =
    "<script>"
    "function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;')"
    ".replace(/>/g,'&gt;').replace(/\"/g,'&quot;')}"
    "function m(t,c){document.getElementById('msg').innerHTML="
    "'<div class=msg '+c+'>'+t+'</div>';"
    "if(c!='err')setTimeout(function(){document.getElementById('msg').innerHTML=''},5000)}"
    "async function conn(){"
    "var s=document.getElementById('ssid').value.trim();if(!s)return m('\\u8bf7\\u586b\\u5199WiFi\\u540d\\u79f0','err');"
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
    "</script></body></html>";

// ================================================================
//  处理 GET / — 返回配网页面
//  这是用户访问 ESP32 时看到的默认页面
// ================================================================
static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, s_html_pre);
    httpd_resp_sendstr_chunk(req, s_html_post);
    httpd_resp_sendstr_chunk(req, "");
    return ESP_OK;
}

// ================================================================
//  处理 GET /api/status — 返回设备状态（JSON）
//  配网页面的 JavaScript 用这个接口获取 AP 名称
// ================================================================
static esp_err_t handle_status(httpd_req_t *req)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "status", "waiting");
    cJSON_AddStringToObject(doc, "ap", s_ap_ssid);  // 返回当前 AP 名称
    char *json = cJSON_PrintUnformatted(doc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(doc);
    return ESP_OK;
}

// ================================================================
//  处理 POST /api/config — 接收 WiFi 配置
//  用户点击"连接 WiFi"按钮后：
//   1) 接收 JSON 格式的 {ssid, password}
//   2) 保存到 NVS
//   3) 等待 500ms
//   4) 重启设备（重启后设备会尝试连接 WiFi）
//  注意：必须先拷贝字符串再 cJSON_Delete，避免使用悬空指针
// ================================================================
static esp_err_t handle_config_post(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // 解析 JSON
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

    // 【重要】先复制字符串，再释放 cJSON，否则 ssid/pass 会变成悬空指针
    char local_ssid[64] = {}, local_pass[64] = {};
    strlcpy(local_ssid, ssid, sizeof(local_ssid));
    strlcpy(local_pass, pass, sizeof(local_pass));
    cJSON_Delete(doc);

    ESP_LOGI(TAG, "Config: SSID=%s", local_ssid);
    save_config(local_ssid, local_pass);  // 写入 NVS

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"rebooting\":true}");

    vTaskDelay(pdMS_TO_TICKS(500));   // 等响应发送完成
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();                     // 重启设备
    return ESP_OK;
}

// ================================================================
//  处理任意 GET 请求 — 统一返回配网页面
//  当手机访问任何网址时（如 baidu.com），DNS 解析到 192.168.4.1，
//  浏览器请求的是 baidu.com 的路径（/），但无论路径是什么，
//  我们都返回配网页面，确保用户能看到配网界面
// ================================================================
static esp_err_t handle_404(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, s_html_pre);
    httpd_resp_sendstr_chunk(req, s_html_post);
    httpd_resp_sendstr_chunk(req, "");
    return ESP_OK;
}

// ================================================================
//  拒绝所有非 GET 请求（CONNECT/POST/PUT/DELETE 等）
//  微信、iOS 等会在连接 WiFi 后发送各种探测请求，
//  这些请求会占用 HTTP 服务器的连接数。
//  我们直接返回 405 并关闭连接，快速释放 socket。
// ================================================================
static esp_err_t handle_deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "");
    return ESP_FAIL;  // 返回失败促使服务器关闭连接
}

// ================================================================
//  启动 HTTP 服务器（由 main_task 在进入 AP 模式后调用）
//  注册所有路由处理器，然后启动 DNS 服务器
// ================================================================
void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;       // 开启 LRU 回收，连接数满时自动关闭最旧的连接
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 10;         // 默认 7，微信 mmtls 占用多需扩容
    cfg.backlog_conn = 7;              // 默认 5
    if (httpd_start(&s_http_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed");
        return;
    }

    // 注册路由
    httpd_uri_t hu;
    hu = (httpd_uri_t){ .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    hu = (httpd_uri_t){ .uri = "/api/status", .method = HTTP_GET, .handler = handle_status, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    hu = (httpd_uri_t){ .uri = "/api/config", .method = HTTP_POST, .handler = handle_config_post, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    hu = (httpd_uri_t){ .uri = "/*", .method = HTTP_GET, .handler = handle_404, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    // 所有非 GET 请求（微信 mmtls CONNECT、杂项 POST/PUT/DELETE）→ 立即关闭释放 socket
    hu = (httpd_uri_t){ .uri = "/*", .method = (httpd_method_t)HTTP_ANY, .handler = handle_deny, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);

    // 启动 DNS 服务器（捕获门户）
    start_dns_server();

    ESP_LOGI(TAG, "HTTP server on port 80 + DNS captive portal");
}

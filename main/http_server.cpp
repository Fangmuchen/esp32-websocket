#include "globals.h"
#include "http_server.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "dns_server.h"
#include <cstring>
#include <cstdlib>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "ledctrl";

httpd_handle_t s_http_server = NULL;

// ========== HTML: 手动输入 SSID + 密码的配网界面 ==========
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

    char local_ssid[64] = {}, local_pass[64] = {};
    strlcpy(local_ssid, ssid, sizeof(local_ssid));
    strlcpy(local_pass, pass, sizeof(local_pass));
    cJSON_Delete(doc);

    ESP_LOGI(TAG, "Config: SSID=%s", local_ssid);
    save_config(local_ssid, local_pass);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"rebooting\":true}");

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
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

static esp_err_t handle_deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "");
    return ESP_FAIL;
}

void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 10;
    cfg.backlog_conn = 7;
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
    hu = (httpd_uri_t){ .uri = "/*", .method = HTTP_GET, .handler = handle_404, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);
    hu = (httpd_uri_t){ .uri = "/*", .method = (httpd_method_t)HTTP_ANY, .handler = handle_deny, .user_ctx = NULL };
    httpd_register_uri_handler(s_http_server, &hu);

    start_dns_server();

    ESP_LOGI(TAG, "HTTP server on port 80 + DNS captive portal");
}

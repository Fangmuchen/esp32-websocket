#include "dns_server.h"
#include <cstring>
#include "esp_log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * dns_server.cpp — DNS 服务器（实现）
 * ====================================
 *
 * 【工作原理】
 * 这是一个"伪 DNS 服务器"，不真的做域名解析。
 * 不管手机查询什么域名（例如 www.baidu.com, example.com...），
 * 我们都返回 192.168.4.1（ESP32 自己的 IP）。
 *
 * 这样手机浏览器就会直接打开 ESP32 上的配网页面。
 *
 * 【DNS 协议简析】
 * DNS 查询包的结构：
 *   - 12 字节头部（事务ID + 标志 + 问题数 + 回答数 + ...）
 *   - 问题区（要查询的域名 + 查询类型）
 *   - 回答区（解析结果）
 *
 * 我们组装一个"假的"回答，把 A 记录指向 192.168.4.1。
 *
 * 【为什么用 BSD socket 不用 ESP-IDF 的 DNS 组件？】
 * 因为我们需要完全控制 DNS 响应来劫持所有域名。
 * ESP-IDF 的 DNS 组件是用于"查询"DNS 的，不是用于"提供"DNS 服务的。
 */

static const char *TAG = "ledctrl";

// ================================================================
//  DNS 服务器任务（在独立 FreeRTOS 任务中运行）
//  监听 UDP 53 端口，处理 DNS 查询
// ================================================================
static void dns_server_task(void *pvParameters)
{
    // 1) 创建 UDP socket，监听 53 端口
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
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 监听所有网络接口
    addr.sin_port = htons(53);                  // DNS 标准端口

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "DNS: bind port 53 failed (already in use?)");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS server ready (captive portal)");

    uint8_t buf[512];   // 接收缓冲区
    uint8_t resp[512];  // 发送缓冲区

    // 2) 循环接收和处理 DNS 查询
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                          (struct sockaddr *)&from, &fromlen);
        if (len < 12) continue;  // DNS 头部至少 12 字节，非法包跳过

        // 复制查询包作为响应的基础（保留事务 ID 等）
        memcpy(resp, buf, len);

        // 设置 DNS 响应标志：
        //   bit15=1 (QR=响应), bit10=1 (AA=权威回答), bit7=1 (RA=支持递归)
        resp[2] = 0x85 | (buf[2] & 0x01);
        resp[3] = 0x80;

        // 3) 解析查询中的域名结束位置（QNAME 的结尾）
        //    DNS 域名格式: 长度+标签 ... 0x00 结尾
        int qend = 12;  // 跳过头部
        while (qend < len && buf[qend] != 0) {
            if ((buf[qend] & 0xC0) == 0xC0) { qend += 2; break; }  // 指针压缩
            qend += buf[qend] + 1;
        }
        if (qend >= len) continue;
        qend += 1;  // 跳过结尾 0x00
        if (qend + 4 > len) continue;

        // 4) 判断查询类型
        uint16_t qtype = (buf[qend] << 8) | buf[qend + 1];
        if (qtype == 1) {
            // A 记录查询（最常见的，查询 IPv4 地址）
            // → 在回答区插入一个伪造的 A 记录，指向 192.168.4.1
            uint8_t *ans = resp + qend + 4;
            ans[0] = 0xC0; ans[1] = 0x0C;    // Name 指针指向问题区
            ans[2] = 0x00; ans[3] = 0x01;    // Type: A
            ans[4] = 0x00; ans[5] = 0x01;    // Class: IN
            ans[6] = 0x00; ans[7] = 0x00;    // TTL: 60 秒
            ans[8] = 0x00; ans[9] = 0x3C;
            ans[10] = 0x00; ans[11] = 0x04;  // 数据长度: 4 字节 (IPv4)
            ans[12] = 192; ans[13] = 168;    // IP: 192.168.4.1
            ans[14] = 4;   ans[15] = 1;
            resp[6] = 0x00; resp[7] = 0x01;  // ANCOUNT = 1（1 个回答）
            sendto(sock, resp, qend + 4 + 16, 0,
                  (struct sockaddr *)&from, fromlen);
        } else {
            // 非 A 记录查询（如 AAAA 查询 IPv6），返回空回答
            resp[7] = 0x00;
            sendto(sock, resp, len, 0,
                  (struct sockaddr *)&from, fromlen);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

// ================================================================
//  启动 DNS 服务器（创建独立任务）
//  由 http_server 的 start_http_server() 调用
// ================================================================
void start_dns_server(void)
{
    xTaskCreate(dns_server_task, "dns", 3072, NULL, 3, NULL);
}

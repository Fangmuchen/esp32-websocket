#include "dns_server.h"
#include <cstring>
#include "esp_log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ledctrl";

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

        memcpy(resp, buf, len);

        resp[2] = 0x85 | (buf[2] & 0x01);
        resp[3] = 0x80;

        int qend = 12;
        while (qend < len && buf[qend] != 0) {
            if ((buf[qend] & 0xC0) == 0xC0) { qend += 2; break; }
            qend += buf[qend] + 1;
        }
        if (qend >= len) continue;
        qend += 1;
        if (qend + 4 > len) continue;

        uint16_t qtype = (buf[qend] << 8) | buf[qend + 1];
        if (qtype == 1) {
            uint8_t *ans = resp + qend + 4;
            ans[0] = 0xC0; ans[1] = 0x0C;
            ans[2] = 0x00; ans[3] = 0x01;
            ans[4] = 0x00; ans[5] = 0x01;
            ans[6] = 0x00; ans[7] = 0x00;
            ans[8] = 0x00; ans[9] = 0x3C;
            ans[10] = 0x00; ans[11] = 0x04;
            ans[12] = 192; ans[13] = 168;
            ans[14] = 4;   ans[15] = 1;
            resp[6] = 0x00; resp[7] = 0x01;
            sendto(sock, resp, qend + 4 + 16, 0,
                  (struct sockaddr *)&from, fromlen);
        } else {
            resp[7] = 0x00;
            sendto(sock, resp, len, 0,
                  (struct sockaddr *)&from, fromlen);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

void start_dns_server(void)
{
    xTaskCreate(dns_server_task, "dns", 3072, NULL, 3, NULL);
}

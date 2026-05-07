#include "globals.h"
#include "led_control.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * led_control.cpp — WS2812 智能 LED 控制（实现）
 * ==============================================
 *
 * 【原理简介】
 * WS2812 灯珠通过一根数据线串联，每个灯珠内部有 IC，
 * 接收 24bit 数据（分别是 Green、Red、Blue 各 8bit）。
 * 数据以不同占空比的方波表示 0 和 1：
 *   - 0: 高电平 0.4us + 低电平 0.8us
 *   - 1: 高电平 0.7us + 低电平 0.6us
 *
 * ESP32 的 RMT 模块可以精确产生这种时序信号。
 * 我们配置 RMT 工作在 10MHz，1 个 tick = 0.1us。
 *
 * 注意：WS2812 的颜色顺序是 GRB（绿红蓝），不是常见的 RGB！
 */

static const char *TAG = "ledctrl";

bool s_led_state = false;           // LED 开关状态（true=亮，false=灭）

static rmt_channel_handle_t s_led_chan = NULL;     // RMT 通道句柄
static rmt_encoder_handle_t s_led_encoder = NULL;  // RMT 编码器句柄

// ================================================================
//  初始化 RMT 模块，配置 WS2812 的通信时序
// ================================================================
esp_err_t led_init(void)
{
    // 1) 配置 RMT 发送通道
    //    时钟源用默认，分辨率 10MHz，64 个内存符号，1 个待发送队列
    rmt_tx_channel_config_t tx_chan = {
        .gpio_num = (gpio_num_t)LED_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_chan, &s_led_chan), TAG, "create RMT TX ch");

    // 2) 配置字节编码器，定义 WS2812 的 0 和 1 波形
    //    bit0: 高4tick(0.4us) + 低8tick(0.8us)  → 代表 0
    //    bit1: 高7tick(0.7us) + 低6tick(0.6us)  → 代表 1
    //    先发送高位（MSB first）
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

    // 3) 启动 RMT 通道
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_chan), TAG, "enable RMT ch");
    return ESP_OK;
}

// ================================================================
//  底层发送函数：直接设置 WS2812 的 RGB 颜色
//  注意参数顺序是 (R, G, B) 但 WS2812 实际需要 GRB 顺序
// ================================================================
void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    // WS2812 的数据顺序是 Green → Red → Blue
    uint8_t data[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s_led_chan, s_led_encoder, data, sizeof(data), &tx_cfg);
    rmt_tx_wait_all_done(s_led_chan, portMAX_DELAY);
}

// ================================================================
//  开关 LED（开=白色，关=熄灭）
// ================================================================
void set_led(bool on)
{
    s_led_state = on;
    if (on)
        ws2812_set_color(255, 255, 255);  // R=255 G=255 B=255 → 白光
    else
        ws2812_set_color(0, 0, 0);        // 全 0 → 熄灭
    ESP_LOGI(TAG, "LED %s", on ? "ON (white)" : "OFF");
}

// ================================================================
//  设置 LED 为任意 RGB 颜色
//  只要 R/G/B 不全为 0，就认为 LED 是"亮"的状态
// ================================================================
void set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_led_state = (r | g | b) != 0;  // 不全为 0 就视为亮
    ws2812_set_color(r, g, b);
    ESP_LOGI(TAG, "LED color: R=%d G=%d B=%d", r, g, b);
}

// ================================================================
//  闪烁 LED 指定次数
//  count    — 闪烁次数
//  delay_ms — 每次亮/灭的持续时间（毫秒）
//  例如: blink_led(3, 200) → 亮200ms 灭200ms 亮200ms 灭200ms 亮200ms 灭
// ================================================================
void blink_led(int count, int delay_ms)
{
    for (int i = 0; i < count; i++) {
        ws2812_set_color(255, 255, 255);         // 亮白色
        vTaskDelay(pdMS_TO_TICKS(delay_ms));     // 等待
        ws2812_set_color(0, 0, 0);               // 灭
        if (i < count - 1)
            vTaskDelay(pdMS_TO_TICKS(delay_ms)); // 除了最后一次，灭的时候也等一会儿
    }
}

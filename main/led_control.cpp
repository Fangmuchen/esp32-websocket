#include "globals.h"
#include "led_control.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ledctrl";

bool s_led_state = false;

static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;

esp_err_t led_init(void)
{
    rmt_tx_channel_config_t tx_chan = {
        .gpio_num = (gpio_num_t)LED_PIN,
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

void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t data[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s_led_chan, s_led_encoder, data, sizeof(data), &tx_cfg);
    rmt_tx_wait_all_done(s_led_chan, portMAX_DELAY);
}

void set_led(bool on)
{
    s_led_state = on;
    if (on)
        ws2812_set_color(255, 255, 255);
    else
        ws2812_set_color(0, 0, 0);
    ESP_LOGI(TAG, "LED %s", on ? "ON (white)" : "OFF");
}

void set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_led_state = (r | g | b) != 0;
    ws2812_set_color(r, g, b);
    ESP_LOGI(TAG, "LED color: R=%d G=%d B=%d", r, g, b);
}

void blink_led(int count, int delay_ms)
{
    for (int i = 0; i < count; i++) {
        ws2812_set_color(255, 255, 255);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        ws2812_set_color(0, 0, 0);
        if (i < count - 1)
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

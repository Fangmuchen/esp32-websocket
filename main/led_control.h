#pragma once

#include <cstdint>
#include "esp_err.h"

extern bool s_led_state;

esp_err_t led_init(void);
void set_led(bool on);
void set_led_color(uint8_t r, uint8_t g, uint8_t b);
void blink_led(int count, int delay_ms);
void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b);

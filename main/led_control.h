#pragma once

/*
 * led_control.h — WS2812 智能 LED 控制（头文件）
 * ==============================================
 * WS2812 是一种"单总线"RGB LED，只需要一根数据线就能控制颜色。
 * ESP32 通过 RMT（红外遥控）模块来生成 WS2812 需要的精确时序信号。
 *
 * 对外提供的函数：
 *   led_init()         — 初始化 RMT 硬件，配置好 WS2812 的通信时序
 *   set_led(on)        — 开关 LED（开=白光，关=灭）
 *   set_led_color(r,g,b) — 设置任意 RGB 颜色
 *   blink_led(次数, 间隔) — 让 LED 闪烁指定次数
 *   ws2812_set_color()   — 底层函数，直接发送 RGB 数据到灯珠
 */

#include <cstdint>
#include "esp_err.h"

extern bool s_led_state;  // 记录 LED 当前是开还是关（true=亮，false=灭）

esp_err_t led_init(void);
void set_led(bool on);
void set_led_color(uint8_t r, uint8_t g, uint8_t b);
void blink_led(int count, int delay_ms);
void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b);

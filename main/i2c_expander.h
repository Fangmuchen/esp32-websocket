#pragma once

/*
 * i2c_expander.h — PCF8575 I2C 扩展 IO 驱动（头文件）
 * ==================================================
 * 双 PCF8575 方案，分开控制灯光和锁球器：
 *   #1 (0x20): P0-P7 → 8 路锁球器
 *   #2 (0x21): P0-P7 → 8 路灯光
 */

#include <cstdint>
#include "esp_err.h"

#define PCF8575_LOCKER_ADDR  0x20   // 锁球器 (A0=A1=A2=GND)
#define PCF8575_LIGHT_ADDR   0x21   // 灯光 (A0=3.3V)
#define PCF8575_NUM_PINS     8      // 各芯片实际使用的引脚数

esp_err_t expander_init(void);
esp_err_t expander_write_locker_pin(uint8_t pin, bool level);
esp_err_t expander_write_light_pin(uint8_t pin, bool level);

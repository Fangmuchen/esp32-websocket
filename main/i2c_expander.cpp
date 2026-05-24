#include "i2c_expander.h"
#include "globals.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"

/*
 * i2c_expander.cpp — 双 PCF8575 I2C 扩展 IO 驱动（实现）
 * =======================================================
 *
 * PCF8575 #1 (0x20) → 8 路锁球器（P0-P7）
 * PCF8575 #2 (0x21) → 8 路灯光（P0-P7）
 *
 * 写入 0 → 引脚输出低电平（继电器触发：灯亮/开锁）
 * 写入 1 → 引脚输出高电平（继电器断开：灯灭/闭锁）
 * 初始状态：全 1
 */

static const char *TAG = "ledctrl";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_locker_dev = NULL;
static i2c_master_dev_handle_t s_light_dev = NULL;
static uint16_t s_locker_out = 0xFFFF;
static uint16_t s_light_out = 0xFFFF;

static esp_err_t write_all(i2c_master_dev_handle_t dev, uint16_t *shadow, uint16_t mask)
{
    if (!dev) return ESP_ERR_INVALID_STATE;
    uint8_t data[2] = { (uint8_t)(mask & 0xFF), (uint8_t)((mask >> 8) & 0xFF) };
    esp_err_t ret = i2c_master_transmit(dev, data, sizeof(data), -1);
    if (ret == ESP_OK) *shadow = mask;
    return ret;
}

static esp_err_t write_pin(i2c_master_dev_handle_t dev, uint16_t *shadow, uint8_t pin, bool level)
{
    if (pin >= PCF8575_NUM_PINS) return ESP_ERR_INVALID_ARG;
    if (level)
        *shadow |= (1 << pin);
    else
        *shadow &= ~(1 << pin);
    return write_all(dev, shadow, *shadow);
}

esp_err_t expander_init(void)
{
    // 1) 创建 I2C 主总线
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "create I2C bus");

    // 2) 锁球器 PCF8575 (0x20)
    i2c_device_config_t locker_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8575_LOCKER_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &locker_cfg, &s_locker_dev),
                        TAG, "add locker PCF8575");
    s_locker_out = 0xFFFF;
    ESP_RETURN_ON_ERROR(write_all(s_locker_dev, &s_locker_out, s_locker_out),
                        TAG, "locker PCF8575 init");

    // 3) 灯光 PCF8575 (0x21)
    i2c_device_config_t light_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8575_LIGHT_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &light_cfg, &s_light_dev),
                        TAG, "add light PCF8575");
    s_light_out = 0xFFFF;
    ESP_RETURN_ON_ERROR(write_all(s_light_dev, &s_light_out, s_light_out),
                        TAG, "light PCF8575 init");

    ESP_LOGI(TAG, "Dual PCF8575 ready: lockers@0x%02X, lights@0x%02X",
             PCF8575_LOCKER_ADDR, PCF8575_LIGHT_ADDR);
    return ESP_OK;
}

esp_err_t expander_write_locker_pin(uint8_t pin, bool level)
{
    return write_pin(s_locker_dev, &s_locker_out, pin, level);
}

esp_err_t expander_write_light_pin(uint8_t pin, bool level)
{
    return write_pin(s_light_dev, &s_light_out, pin, level);
}

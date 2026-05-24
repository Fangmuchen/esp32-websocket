#include "voice_player.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "voice";
static const uart_port_t UART_NUM = UART_NUM_2;

// DFPlayer Mini 协议：SOF VER LEN CMD FB DH DL CHK EOF
// SOF=0x7E VER=0xFF LEN=0x06 CMD=command FB=0x00 DH/DL=data CHK=checksum EOF=0xEF
#define SOF 0x7E
#define VER 0xFF
#define LEN 0x06
#define EOF_BYTE 0xEF

static void send_cmd(uint8_t cmd, uint16_t data)
{
    uint8_t buf[10];
    uint8_t chk = 0xFFFF - (VER + LEN + cmd + 0x00 + (data >> 8) + (data & 0xFF)) + 1;

    buf[0] = SOF;
    buf[1] = VER;
    buf[2] = LEN;
    buf[3] = cmd;
    buf[4] = 0x00;          // no feedback
    buf[5] = data >> 8;     // data high
    buf[6] = data & 0xFF;   // data low
    buf[7] = chk >> 8;
    buf[8] = chk & 0xFF;
    buf[9] = EOF_BYTE;

    uart_write_bytes(UART_NUM, buf, sizeof(buf));
}

void voice_init(void)
{
    uart_config_t cfg = {
        .baud_rate = VOICE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, VOICE_UART_TX_PIN, VOICE_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, 256, 0, 0, NULL, 0));

    // 选择 SD 卡作为音源
    send_cmd(0x09, 0x0002);
    vTaskDelay(pdMS_TO_TICKS(200));

    // 设音量
    voice_set_volume(VOICE_DEFAULT_VOLUME);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "DFPlayer initialized (TX:GPIO%d, volume=%d)",
             VOICE_UART_TX_PIN, VOICE_DEFAULT_VOLUME);
}

void voice_set_volume(uint8_t vol)
{
    if (vol > 30) vol = 30;
    send_cmd(0x06, vol);
}

void voice_play(uint8_t file_id)
{
    if (file_id < 1 || file_id > 255) return;
    ESP_LOGI(TAG, "Play track %d", file_id);
    send_cmd(0x03, file_id);
}

void voice_stop(void)
{
    send_cmd(0x16, 0);
}

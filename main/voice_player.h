#pragma once

#include <cstdint>
#include "driver/gpio.h"

#define VOICE_UART_TX_PIN      GPIO_NUM_17
#define VOICE_UART_RX_PIN      GPIO_NUM_18
#define VOICE_UART_BAUD        9600
#define VOICE_DEFAULT_VOLUME   25

void voice_init(void);
void voice_set_volume(uint8_t vol);
void voice_play(uint8_t file_id);   // file_id: 1-255
void voice_stop(void);

#pragma once

#include <cstdint>
#include "cJSON.h"

#define NUM_LIGHT_CHANNELS  8
#define NUM_AUDIO_CHANNELS  8

typedef struct {
    bool lights[NUM_LIGHT_CHANNELS];
    bool audio[NUM_AUDIO_CHANNELS];
    bool pool_table;
} control_state_t;

typedef struct {
    bool processed;
    bool interval_ok;     // true = success
    bool light_ok;        // true = success, false = fail/not-applicable
    bool audio_ok;        // true = success, false = fail/not-applicable
} command_result_t;

void control_init(void);
void control_set_light(int channel, bool on);
void control_set_audio(int channel, bool on);
void control_set_pool_table(bool on);
bool control_get_pool_table(void);
void control_get_light_str(char *buf, size_t len);
void control_get_audio_str(char *buf, size_t len);

// Parse a "setting" command from platform, update state, return result codes
command_result_t control_process_command(cJSON *root);

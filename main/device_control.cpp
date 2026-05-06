#include "device_control.h"
#include <cstring>
#include <cstdlib>
#include "esp_log.h"

static const char *TAG = "device";

static control_state_t s_state;

void control_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "Control initialized (8 lights, 8 audio, pool table)");
}

void control_set_light(int channel, bool on)
{
    if (channel < 0 || channel >= NUM_LIGHT_CHANNELS) return;
    if (s_state.lights[channel] != on) {
        s_state.lights[channel] = on;
        ESP_LOGI(TAG, "Light %d: %s", channel + 1, on ? "ON" : "OFF");
        // TODO: actual GPIO/relay control per channel
    }
}

void control_set_audio(int channel, bool on)
{
    if (channel < 0 || channel >= NUM_AUDIO_CHANNELS) return;
    if (s_state.audio[channel] != on) {
        s_state.audio[channel] = on;
        ESP_LOGI(TAG, "Audio %d: %s", channel + 1, on ? "ON" : "OFF");
        // TODO: actual GPIO/relay control per channel
    }
}

void control_set_pool_table(bool on)
{
    if (s_state.pool_table != on) {
        s_state.pool_table = on;
        ESP_LOGI(TAG, "Pool table: %s", on ? "ON" : "OFF");
        // TODO: actual GPIO/relay control for pool table power
    }
}

bool control_get_pool_table(void)
{
    return s_state.pool_table;
}

void control_get_light_str(char *buf, size_t len)
{
    if (len < (size_t)NUM_LIGHT_CHANNELS + 1) return;
    for (int i = 0; i < NUM_LIGHT_CHANNELS; i++)
        buf[i] = s_state.lights[i] ? '1' : '0';
    buf[NUM_LIGHT_CHANNELS] = '\0';
}

void control_get_audio_str(char *buf, size_t len)
{
    if (len < (size_t)NUM_AUDIO_CHANNELS + 1) return;
    for (int i = 0; i < NUM_AUDIO_CHANNELS; i++)
        buf[i] = s_state.audio[i] ? '1' : '0';
    buf[NUM_AUDIO_CHANNELS] = '\0';
}

command_result_t control_process_command(cJSON *root)
{
    command_result_t res = { .processed = false, .interval_ok = false, .light_ok = true, .audio_ok = true };

    if (!root) return res;

    cJSON *ti = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(ti)) return res;
    const char *type = ti->valuestring;

    if (strcmp(type, "setting") == 0) {
        res.processed = true;

        // —— Light control: lightNo (0-7) + lightStatus (0/1) ——
        cJSON *ln = cJSON_GetObjectItem(root, "lightNo");
        cJSON *ls = cJSON_GetObjectItem(root, "lightStatus");
        if (cJSON_IsNumber(ln) && cJSON_IsNumber(ls)) {
            int ch = ln->valueint;       // 0-7
            if (ch >= 0 && ch < NUM_LIGHT_CHANNELS && (ls->valueint == 0 || ls->valueint == 1)) {
                control_set_light(ch, ls->valueint != 0);
                res.light_ok = true;
            } else {
                res.light_ok = false;
                ESP_LOGW(TAG, "Invalid light: No=%d, Status=%d", ch, ls->valueint);
            }
        }

        // —— Audio control: audioNo (0=only) + audioStatus (0/1) ——
        cJSON *an = cJSON_GetObjectItem(root, "audioNo");
        cJSON *as = cJSON_GetObjectItem(root, "audioStatus");
        if (cJSON_IsNumber(an) && cJSON_IsNumber(as)) {
            if (an->valueint == 0 && (as->valueint == 0 || as->valueint == 1)) {
                control_set_audio(0, as->valueint != 0);
                res.audio_ok = true;
            } else {
                res.audio_ok = false;
                ESP_LOGW(TAG, "Invalid audio: No=%d, Status=%d (No must be 0, Status must be 0/1)",
                         an->valueint, as->valueint);
            }
        }

        // Pool table: tableStatus (0/1)
        cJSON *ts = cJSON_GetObjectItem(root, "tableStatus");
        if (cJSON_IsNumber(ts)) {
            if (ts->valueint == 0 || ts->valueint == 1) {
                control_set_pool_table(ts->valueint != 0);
            } else {
                ESP_LOGW(TAG, "Invalid tableStatus: %d", ts->valueint);
            }
        }
    }

    return res;
}

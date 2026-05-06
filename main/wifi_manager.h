#pragma once

#include <cstdint>
#include "esp_event.h"

extern char s_ap_ssid[32];

void wifi_init_event_group(void);
void wifi_event_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data);
bool connect_to_wifi(const char *ssid, const char *pass);
void start_ap_mode(void);

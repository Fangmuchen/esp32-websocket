#pragma once

extern char s_saved_ssid[64];
extern char s_saved_pass[64];

bool ssid_is_valid(const char *ssid);
void save_config(const char *ssid, const char *pass);
bool load_config(void);
void clear_config(void);

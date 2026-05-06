#pragma once

#include "esp_websocket_client.h"

extern esp_websocket_client_handle_t s_ws_client;

void connect_websocket(void);
bool ws_is_connected(void);
void ws_tick(void);
void ws_set_heartbeat_interval(int seconds);

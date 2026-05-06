#pragma once

#include "esp_http_server.h"

extern httpd_handle_t s_http_server;

void start_http_server(void);

idf_component_register(SRCS main.cpp
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_websocket_client json esp_driver_rmt driver)

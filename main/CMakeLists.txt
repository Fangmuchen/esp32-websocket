idf_component_register(SRCS main.cpp
                            led_control.cpp
                            config_manager.cpp
                            wifi_manager.cpp
                            dns_server.cpp
                            http_server.cpp
                            websocket_client.cpp
                            device_control.cpp
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_websocket_client json esp_driver_rmt driver)

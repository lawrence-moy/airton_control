idf_component_register(
    SRCS "main.cpp" "ota.cpp" "discovery.cpp" "tuya_mcu.cpp" "acw02.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash esp_http_server app_update esp_timer lwip)

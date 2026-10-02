idf_component_register(SRCS "ble_config_service.cpp" "network_scanner.cpp" "main.cpp" "ws2812_controller.cpp" "app_config.cpp" "wifi_manager.cpp" "web_server.cpp" "system_monitor.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash spiffs esp_timer spi_flash heap lwip led_strip bt espressif__cjson)

# 把 ../data 目录（含 wifi.json）打包为 SPIFFS 镜像，随 idf.py flash 一起烧录
spiffs_create_partition_image(storage ../data FLASH_IN_PROJECT)

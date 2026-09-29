idf_component_register(SRCS "main.cpp" "app_config.cpp" "wifi_manager.cpp" "web_server.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash spiffs)

# 把 ../data 目录（含 wifi.json）打包为 SPIFFS 镜像，随 idf.py flash 一起烧录
spiffs_create_partition_image(storage ../data FLASH_IN_PROJECT)

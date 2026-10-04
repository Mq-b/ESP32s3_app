idf_component_register(SRCS "barcode_worker.cpp" "barcode_decoder.cpp" "barcode_scan_server.cpp" "ble_config_service.cpp" "network_scanner.cpp" "main.cpp" "ws2812_controller.cpp" "app_config.cpp" "wifi_manager.cpp" "web_server.cpp" "web_assets.cpp" "system_monitor.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash spiffs esp_timer spi_flash heap lwip led_strip bt mittelab__nlohmann-json zxing stb_image barcode_runtime)

# 把 ../data 目录（含 index.html 和 wifi.json）打包为 SPIFFS 镜像，随 idf.py flash 一起烧录
spiffs_create_partition_image(storage ../data FLASH_IN_PROJECT)

# ZXing 抛出输入与分配异常；仅扫码实现使用异常语法。
set_source_files_properties(barcode_worker.cpp barcode_decoder.cpp barcode_scan_server.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")

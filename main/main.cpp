#include "app_config.h"
#include "barcode_scan_server.h"
#include "esp_heap_caps.h"
#include "ble_config_service.h"
#include "network_scanner.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "ws2812_controller.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <memory>

static const char *TAG = "APP";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-S3 启动");

    // NVS 是 WiFi 驱动的依赖
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    static auto config = std::make_unique<AppConfig>(AppConfig::load());
    err = BleConfigService::start(*config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙配网服务启动失败: %s", esp_err_to_name(err));
    }
    static auto wifi = std::make_unique<WiFiManager>(*config);
    wifi->start();

    // HTTP 服务（含设备扫描 API），扫描由页面触发
    static auto led = std::make_unique<Ws2812Controller>(48);
    static auto server = std::make_unique<WebServer>(NetworkScanner::instance(), *led);
    server->start(config->staticIp);
    ESP_LOGI(TAG, "PSRAM 可用堆: %u 字节", static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
    err = BarcodeScanServer::start();
    if (err != ESP_OK) ESP_LOGE(TAG, "扫码服务启动失败: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "启动完成，主任务栈历史最小余量: %u 字节",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

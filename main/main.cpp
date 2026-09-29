#include "app_config.hpp"
#include "web_server.hpp"
#include "wifi_manager.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "APP";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-S3 启动");

    // NVS 是 WiFi 驱动的依赖
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 1. 从 SPIFFS 的 /spiffs/wifi.json 加载配置
    AppConfig config = loadAppConfig();

    // 2. 连接 WiFi（固定 IP，断线自动重连）
    wifiStart(config);

    // 3. 启动 HTTP 服务（80 端口）
    startWebServer(config.staticIp);

    // 主线程无需轮询，事件驱动即可
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

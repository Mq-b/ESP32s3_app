#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

static const char *TAG = "WIFI";

namespace {

/**
 * @brief WiFi 事件处理：启动即连接，断线自动重连
 */
void onWifiEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 断开，3 秒后重连...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "已获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

/**
 * @brief 关闭 DHCP 并应用固定 IP
 */
void applyStaticIp(esp_netif_t *netif, const AppConfig &config) {
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif));

    esp_netif_ip_info_t ipInfo{};
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(config.staticIp.c_str(), &ipInfo.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(config.gateway.c_str(), &ipInfo.gw));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(config.netmask.c_str(), &ipInfo.netmask));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ipInfo));

    ESP_LOGI(TAG, "固定 IP: %s 网关: %s", config.staticIp.c_str(), config.gateway.c_str());
}

}  // namespace

void WiFiManager::start() {
    const AppConfig &config = config_;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    applyStaticIp(netif, config);

    wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initCfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        onWifiEvent, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        onWifiEvent, nullptr, nullptr));

    if (config.ssid.empty()) {
        ESP_LOGW(TAG, "尚未配置 WiFi，保留蓝牙配网服务等待设置");
        return;
    }

    wifi_config_t wifiCfg{};
    std::strncpy(reinterpret_cast<char *>(wifiCfg.sta.ssid), config.ssid.c_str(),
                 sizeof(wifiCfg.sta.ssid));
    std::strncpy(reinterpret_cast<char *>(wifiCfg.sta.password), config.password.c_str(),
                 sizeof(wifiCfg.sta.password));
    wifiCfg.sta.threshold.authmode = config.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiCfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "正在连接 WiFi: %s", config.ssid.c_str());
}

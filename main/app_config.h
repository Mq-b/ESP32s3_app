#pragma once

#include <string>

/**
 * @brief 应用配置：从 SPIFFS 的 /spiffs/wifi.json 加载
 */
class AppConfig {
public:
    std::string ssid;
    std::string password;
    std::string staticIp = "192.168.0.10";
    std::string gateway = "192.168.0.1";
    std::string netmask = "255.255.255.0";

    /**
     * @brief 挂载 SPIFFS 并解析配置文件（解析失败时返回默认值）
     */
    static AppConfig load();
};

#pragma once

#include <string>

/**
 * @brief 应用配置，从 SPIFFS 上的 /spiffs/wifi.json 加载
 */
struct AppConfig {
    std::string ssid;
    std::string password;
    std::string staticIp = "192.168.0.10";
    std::string gateway  = "192.168.0.1";
    std::string netmask  = "255.255.255.0";
};

/**
 * @brief 挂载 SPIFFS (storage 分区) 并解析 /spiffs/wifi.json
 * @return 解析后的配置；文件缺失或字段缺失时使用默认值并打印警告
 */
AppConfig loadAppConfig();

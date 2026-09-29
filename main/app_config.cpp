#include "app_config.hpp"

#include "esp_log.h"
#include "esp_spiffs.h"

#include <cstdio>
#include <string>

static const char *TAG = "CONFIG";

static constexpr const char *CONFIG_PATH = "/spiffs/wifi.json";

/**
 * @brief 挂载 SPIFFS 文件系统
 */
static void mountSpiffs() {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}

/**
 * @brief 读取整个文件内容
 */
static std::string readFile(const char *path) {
    FILE *f = std::fopen(path, "r");
    if (!f) {
        return {};
    }
    std::string content;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
    }
    std::fclose(f);
    return content;
}

/**
 * @brief 极简 JSON 取值：适用于扁平对象中的字符串字段
 *        例: {"ssid": "bigtop", ...}，支持空白与 \" \\ \/ \n \t 转义
 */
static std::string jsonString(const std::string &json, const std::string &key,
                              const std::string &fallback) {
    // 定位 "key"
    size_t pos = json.find('"' + key + '"');
    if (pos == std::string::npos) {
        ESP_LOGW(TAG, "配置缺少字段 \"%s\"，使用默认值 \"%s\"", key.c_str(), fallback.c_str());
        return fallback;
    }

    // 跳过冒号与空白
    pos = json.find(':', pos);
    if (pos == std::string::npos) return fallback;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos || json[pos] != '"') return fallback;
    ++pos;

    // 提取字符串值，处理转义
    std::string value;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char esc = json[++pos];
            switch (esc) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                default:  value += esc;  break;  // \" \\ \/ 等原样保留
            }
            ++pos;
        } else {
            value += json[pos++];
        }
    }
    return value.empty() ? fallback : value;
}

AppConfig loadAppConfig() {
    mountSpiffs();

    AppConfig config;
    std::string json = readFile(CONFIG_PATH);
    if (json.empty()) {
        ESP_LOGE(TAG, "未找到 %s，请烧录 SPIFFS 镜像 (idf.py flash)", CONFIG_PATH);
        return config;
    }

    config.ssid     = jsonString(json, "ssid", "");
    config.password = jsonString(json, "password", "");
    config.staticIp = jsonString(json, "static_ip", config.staticIp);
    config.gateway  = jsonString(json, "gateway", config.gateway);
    config.netmask  = jsonString(json, "netmask", config.netmask);

    ESP_LOGI(TAG, "配置加载完成: ssid=%s ip=%s gw=%s",
             config.ssid.c_str(), config.staticIp.c_str(), config.gateway.c_str());
    return config;
}

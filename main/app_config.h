#pragma once

#include "esp_err.h"

#include <string>

/**
 * @brief 应用网络配置，优先读取蓝牙保存的 NVS 副本，其次读取 SPIFFS 的
 * wifi.json。
 */
class AppConfig {
public:
    std::string ssid;
    std::string password;
    std::string staticIp = "192.168.0.10";
    std::string gateway = "192.168.0.1";
    std::string netmask = "255.255.255.0";

    /**
     * @brief 挂载 SPIFFS 并加载网络配置，失败时保留默认值以便通过蓝牙修复。
     * @return 加载的配置；NVS 必须已经初始化。
     */
    static AppConfig load();

    /**
     * @brief 严格解析并校验完整网络配置，不接受缺失字段或非法网络地址。
     * @param json 包含五个字符串字段的 JSON，最大 1024 字节。
     * @param config 成功时接收配置，失败时不变。
     * @param error 失败原因，不包含密码。
     * @return 配置是否有效。
     */
    static bool parse(const std::string &json, AppConfig &config,
                      std::string &error);

    /**
     * @brief 保存配置到 NVS 并同步 wifi.json，下次启动生效。
     * @return NVS 提交结果；文件同步失败时仍可从 NVS 恢复。
     * @note NVS 必须已经初始化；调用者负责串行写入，不修改当前运行的 WiFi。
     */
    esp_err_t save() const;

    /**
     * @brief 序列化全部配置字段，包含密码，仅供可信存储或认证加密传输使用。
     * @return 完整 JSON；内存分配失败时返回空字符串。
     * @note 不得将返回值写入日志或非认证接口。
     */
    std::string toJson() const;
};

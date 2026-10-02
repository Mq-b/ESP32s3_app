#pragma once

#include "app_config.h"
#include "esp_err.h"

/**
 * @brief BLE 配网服务：广播发现、认证配对、分包配置写入及保存后重启。
 * @note 单连接服务；广播不包含 WiFi 凭据，必须在 NVS 初始化和配置加载后启动。
 */
class BleConfigService {
public:
    /**
     * @brief 启动 NimBLE 配网服务，使用固定配对码 123456。
     * @param config 当前加载的配置，服务保存副本供认证连接读取。
     * @return 初始化结果；一个启动周期只能调用一次。
     */
    static esp_err_t start(const AppConfig &config);
};

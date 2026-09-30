#pragma once

#include "app_config.h"

/**
 * @brief WiFi STA 管理：固定 IP 连接 + 断线自动重连
 */
class WiFiManager {
public:
    explicit WiFiManager(const AppConfig &config) : config_(config) {}

    /// 初始化并以固定 IP 连接（连接过程经事件回调异步进行）
    void start();

private:
    const AppConfig &config_;
};

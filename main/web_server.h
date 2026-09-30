#pragma once

#include "network_scanner.h"

#include <string>

/**
 * @brief HTTP 服务：首页（设备列表）+ 设备/扫描 API
 */
class WebServer {
public:
    explicit WebServer(NetworkScanner &scanner) : scanner_(scanner) {}

    /// 启动服务（80 端口）
    void start(const std::string &deviceIp);

private:
    NetworkScanner &scanner_;
};

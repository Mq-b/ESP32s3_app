#pragma once

#include "network_scanner.h"
#include "ws2812_controller.h"

#include <string>

/**
 * @brief HTTP 服务：首页、内网设备扫描以及板载 WS2812 控制 API。
 */
class WebServer {
public:
    /**
     * @brief 构造 HTTP 服务。
     * @param scanner 用于执行内网扫描的对象，调用期间必须保持有效。
     * @param led 用于控制板载 WS2812 的对象，调用期间必须保持有效。
     */
    WebServer(NetworkScanner &scanner, Ws2812Controller &led) : scanner_(scanner), led_(led) {}

    /**
     * @brief 启动 80 端口 HTTP 服务。
     * @param deviceIp 设备当前 IP 地址，仅用于日志显示。
     */
    void start(const std::string &deviceIp);

private:
    NetworkScanner &scanner_;
    Ws2812Controller &led_;
};

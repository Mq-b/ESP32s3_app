#pragma once

#include <string>

/**
 * @brief 启动 HTTP 服务，监听 80 端口，GET / 返回设备首页
 * @param ip 设备固定 IP，用于页面展示与日志输出
 */
void startWebServer(const std::string &ip);

#pragma once

struct AppConfig;

/**
 * @brief 以 STA 模式连接 WiFi，使用配置中的固定 IP（关闭 DHCP）。
 *        断线后自动重连。
 */
void wifiStart(const AppConfig &config);

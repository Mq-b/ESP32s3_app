#pragma once
#include "esp_err.h"

/** @brief 独立的浏览器扫码服务，串行处理单帧上传，不阻塞 80 端口业务。 */
class BarcodeScanServer {
public:
    /**
     * @brief 启动 81 端口扫码页面和 JPEG 上传接口。
     * @return 启动或路由注册结果，失败时关闭部分启动的服务。
     * @note 须在 SPIFFS 挂载后调用一次；仅适合可信局域网，不具备认证功能。
     */
    static esp_err_t start();
};

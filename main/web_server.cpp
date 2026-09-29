#include "web_server.hpp"

#include "esp_http_server.h"
#include "esp_log.h"

#include <string>

static const char *TAG = "HTTP";

// 页面模板，运行时把 IP 拼进去
static std::string buildIndexHtml(const std::string &ip) {
    return std::string(R"HTML(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-S3</title>
<style>
  body { font-family: system-ui, sans-serif; display: flex; justify-content: center;
         align-items: center; min-height: 100vh; margin: 0; background: #0f172a; color: #e2e8f0; }
  .card { background: #1e293b; border-radius: 16px; padding: 40px 56px; text-align: center;
          box-shadow: 0 10px 40px rgba(0,0,0,.4); }
  h1 { margin: 0 0 8px; color: #38bdf8; }
  p { margin: 4px 0; color: #94a3b8; }
  .ip { font-size: 1.4em; color: #4ade80; font-weight: 600; }
</style>
</head>
<body>
  <div class="card">
    <h1>ESP32-S3 HTTP 服务</h1>
    <p>设备已连接公司 WiFi</p>
    <p>固定 IP 地址</p>
    <p class="ip">)HTML") + ip + R"HTML(</p>
    <p>Powered by ESP-IDF + C++</p>
  </div>
</body>
</html>
)HTML";
}

static std::string g_indexHtml;

/**
 * @brief GET / 返回首页
 */
static esp_err_t indexHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, g_indexHtml.c_str(), g_indexHtml.size());
    return ESP_OK;
}

void startWebServer(const std::string &ip) {
    g_indexHtml = buildIndexHtml(ip);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t indexUri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = indexHandler,
        .user_ctx = nullptr,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &indexUri));

    ESP_LOGI(TAG, "HTTP 服务已启动: http://%s/", ip.c_str());
}

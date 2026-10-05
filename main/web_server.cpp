#include "web_server.h"

#include "web_assets.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>

static const char *TAG = "HTTP";

namespace {

/**
 * @brief 从已挂载的 SPIFFS 按块发送首页，不在内存中复制整个页面。
 * @param req 当前 HTTP 请求。
 * @return 文件读取或 HTTP 响应发送结果。
 */
esp_err_t indexHandler(httpd_req_t *req) {
    return sendHtmlFile(req, "/spiffs/index.html");
}

/**
 * @brief 返回系统资源快照，由 HTTP 服务任务串行访问采集器。
 * @param req 包含 SystemMonitor 上下文的 HTTP 请求。
 * @return HTTP 响应发送结果。
 */
esp_err_t systemHandler(httpd_req_t *req) {
    auto *monitor = static_cast<SystemMonitor *>(req->user_ctx);
    const std::string json = monitor->statusJson();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t devicesHandler(httpd_req_t *req) {
    auto *scanner = static_cast<NetworkScanner *>(req->user_ctx);
    std::string json = scanner->devicesJson();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/**
 * @brief 使用统一 JSON 库序列化并发送 HTTP 响应。
 * @param req 当前 HTTP 请求。
 * @param body 待发送的 JSON 值，字符串必须为合法 UTF-8。
 * @return HTTP 响应发送结果。
 */
esp_err_t sendJsonResponse(httpd_req_t *req,
                           const nlohmann::ordered_json &body) {
    const std::string response = body.dump();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response.c_str(), response.size());
}

esp_err_t scanHandler(httpd_req_t *req) {
    auto *scanner = static_cast<NetworkScanner *>(req->user_ctx);
    scanner->startAsync();
    return sendJsonResponse(req, {{"ok", true}});
}

const char *ledModeName(Ws2812Mode mode) {
    switch (mode) {
    case Ws2812Mode::Off:
        return "off";
    case Ws2812Mode::On:
        return "on";
    case Ws2812Mode::Blink:
        return "blink";
    }
    return "off";
}

bool readColorParameter(const char *query, const char *key, uint8_t &value) {
    char text[4] = {};
    if (httpd_query_key_value(query, key, text, sizeof(text)) != ESP_OK) {
        return false;
    }

    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > 255) {
        return false;
    }
    value = static_cast<uint8_t>(parsed);
    return true;
}

bool readRequestedColor(const char *query, Ws2812Color &color) {
    const bool hasRed = strstr(query, "r=") != nullptr;
    const bool hasGreen = strstr(query, "g=") != nullptr;
    const bool hasBlue = strstr(query, "b=") != nullptr;
    if (!hasRed && !hasGreen && !hasBlue) {
        return true;
    }
    return hasRed && hasGreen && hasBlue &&
           readColorParameter(query, "r", color.red) &&
           readColorParameter(query, "g", color.green) &&
           readColorParameter(query, "b", color.blue);
}

esp_err_t ledStatusHandler(httpd_req_t *req) {
    auto *led = static_cast<Ws2812Controller *>(req->user_ctx);
    const Ws2812Color color = led->color();
    const nlohmann::ordered_json response = {{"mode", ledModeName(led->mode())},
                                             {"r", color.red},
                                             {"g", color.green},
                                             {"b", color.blue}};
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return sendJsonResponse(req, response);
}

esp_err_t ledControlHandler(httpd_req_t *req) {
    char query[64] = {};
    char mode[16] = {};
    auto *led = static_cast<Ws2812Controller *>(req->user_ctx);
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "mode", mode, sizeof(mode)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 mode 参数");
        return ESP_FAIL;
    }

    Ws2812Color color = led->color();
    if (!readRequestedColor(query, color)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "RGB 参数必须为 0 至 255 的整数");
        return ESP_FAIL;
    }
    led->setColor(color);

    if (strcmp(mode, "off") == 0) {
        led->turnOff();
    } else if (strcmp(mode, "on") == 0) {
        led->turnOn(color);
    } else if (strcmp(mode, "blink") == 0) {
        led->startBlinking(color, 500, 500);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "不支持的 mode 参数");
        return ESP_FAIL;
    }

    return sendJsonResponse(req, {{"ok", true}});
}

} // namespace

void WebServer::start(const std::string &deviceIp) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    // JSON 构造和序列化需要额外栈空间，避免 HTTP 请求触发栈溢出。
    config.stack_size = 8192;
    // 两个 HTTP 服务共享全局 socket 池，连接满时回收最久未使用的会话。
    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;
    config.backlog_conn = 4;

    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    auto *scanner = &scanner_;
    auto *led = &led_;

    const httpd_uri_t indexUri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = indexHandler,
        .user_ctx = nullptr,
    };
    const httpd_uri_t devicesUri = {
        .uri = "/api/devices",
        .method = HTTP_GET,
        .handler = devicesHandler,
        .user_ctx = scanner,
    };
    const httpd_uri_t scanUri = {
        .uri = "/api/scan",
        .method = HTTP_POST,
        .handler = scanHandler,
        .user_ctx = scanner,
    };
    const httpd_uri_t ledStatusUri = {
        .uri = "/api/led",
        .method = HTTP_GET,
        .handler = ledStatusHandler,
        .user_ctx = led,
    };
    const httpd_uri_t ledControlUri = {
        .uri = "/api/led",
        .method = HTTP_POST,
        .handler = ledControlHandler,
        .user_ctx = led,
    };

    const httpd_uri_t systemUri = {
        .uri = "/api/system",
        .method = HTTP_GET,
        .handler = systemHandler,
        .user_ctx = &monitor_,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &systemUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &indexUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devicesUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scanUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ledStatusUri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ledControlUri));

    ESP_LOGI(TAG, "HTTP 服务已启动: http://%s/", deviceIp.c_str());
}

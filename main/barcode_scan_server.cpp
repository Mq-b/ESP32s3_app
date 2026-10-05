#include "barcode_scan_server.h"
#include "barcode_worker.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "web_assets.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>

namespace {
constexpr const char *TAG = "BARCODE_HTTP";
constexpr const char *CACHE_PATH = "/spiffs/barcode-upload.tmp";
constexpr size_t MAX_JPEG_BYTES = 1024 * 1024;
constexpr size_t STORAGE_RESERVE = 64 * 1024;

/** @brief 返回扫码页面，页面和 API 同源且不开放 CORS。 */
esp_err_t pageHandler(httpd_req_t *req) {
    return sendHtmlFile(req, "/spiffs/scanner.html");
}

/** @brief 发送错误并关闭连接，避免未消费的请求体污染下一次请求。 */
esp_err_t fail(httpd_req_t *req, const char *status, const char *message) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    // 错误消息均为本模块的固定文本，不含 JSON
    // 引号或反斜杠；错误路径避免堆分配。
    char body[640] = {};
    std::snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
                  message);
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/** @brief 管理可选 Flash 暂存文件，所有返回路径均关闭并删除文件。 */
struct UploadFile {
    FILE *handle = nullptr; /**< 当前上传文件句柄。 */
    bool created = false;   /**< 是否需要删除本次临时文件。 */
    /** @brief 关闭并删除本次上传创建的临时文件。 */
    ~UploadFile() {
        if (handle)
            std::fclose(handle);
        if (created)
            std::remove(CACHE_PATH);
    }
};

/** @brief 有界接收 JPEG，校验类型、长度、编号和总接收时限后串行解码。 */
esp_err_t scanHandler(httpd_req_t *req) {
    try {
        if (req->content_len == 0 || req->content_len > MAX_JPEG_BYTES)
            return fail(req, "413 Payload Too Large",
                        "JPEG 大小必须为 1 至 1 MiB，请降低压缩质量");
        char type[32] = {};
        if (httpd_req_get_hdr_value_str(req, "Content-Type", type,
                                        sizeof(type)) != ESP_OK ||
            std::strcmp(type, "image/jpeg") != 0)
            return fail(req, "415 Unsupported Media Type",
                        "仅接受 image/jpeg 二进制请求体");
        char query[96] = {}, frame[16] = {}, cache[12] = {};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "frameId", frame, sizeof(frame)) !=
                ESP_OK)
            return fail(req, "400 Bad Request", "缺少 frameId 参数");
        for (const char *p = frame; *p; ++p)
            if (*p < '0' || *p > '9')
                return fail(req, "400 Bad Request", "frameId 必须为无符号整数");
        char *end = nullptr;
        const unsigned long long id = std::strtoull(frame, &end, 10);
        if (end == frame || *end || id > UINT32_MAX)
            return fail(req, "400 Bad Request", "frameId 超出范围");
        const bool flash = httpd_query_key_value(query, "cache", cache,
                                                 sizeof(cache)) == ESP_OK &&
                           std::strcmp(cache, "flash") == 0;
        UploadFile file;
        using Buffer = std::unique_ptr<uint8_t, decltype(&heap_caps_free)>;
        Buffer bytes(nullptr, &heap_caps_free);
        if (flash) {
            size_t total = 0, used = 0;
            if (esp_spiffs_info("storage", &total, &used) != ESP_OK ||
                total < used ||
                total - used < req->content_len + STORAGE_RESERVE)
                return fail(req, "507 Insufficient Storage",
                            "Flash 空间不足，保留配置文件空间");
            file.handle = std::fopen(CACHE_PATH, "wb+");
            if (!file.handle)
                return fail(req, "507 Insufficient Storage",
                            "无法创建 Flash 临时缓存");
            file.created = true;
        } else {
            void *p = heap_caps_malloc(req->content_len,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!p &&
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >
                    req->content_len + STORAGE_RESERVE)
                p = heap_caps_malloc(req->content_len, MALLOC_CAP_8BIT);
            bytes.reset(static_cast<uint8_t *>(p));
            if (!bytes)
                return fail(req, "503 Service Unavailable",
                            "上传缓冲内存不足，可尝试 Flash 缓存或检查 PSRAM");
        }
        std::array<char, 2048> chunk{};
        size_t received = 0;
        const int64_t started = esp_timer_get_time();
        while (received < req->content_len) {
            if (esp_timer_get_time() - started > 10 * 1000 * 1000)
                return fail(req, "408 Request Timeout", "上传超过 10 秒");
            const size_t count =
                std::min(chunk.size(), req->content_len - received);
            char *destination =
                flash ? chunk.data()
                      : reinterpret_cast<char *>(bytes.get() + received);
            const int read = httpd_req_recv(req, destination, count);
            if (read <= 0)
                return fail(req, "408 Request Timeout", "图片上传中断或超时");
            if (flash && std::fwrite(chunk.data(), 1, read, file.handle) !=
                             static_cast<size_t>(read))
                return fail(req, "507 Insufficient Storage",
                            "Flash 缓存写入失败");
            received += read;
        }
        if (flash && std::fflush(file.handle) != 0)
            return fail(req, "507 Insufficient Storage", "Flash 缓存刷新失败");
        uint8_t signature[2] = {};
        if (flash) {
            std::rewind(file.handle);
            if (std::fread(signature, 1, 2, file.handle) != 2)
                return fail(req, "400 Bad Request", "JPEG 文件不完整");
        } else if (received >= 2) {
            std::memcpy(signature, bytes.get(), 2);
        }
        if (signature[0] != 0xff || signature[1] != 0xd8)
            return fail(req, "400 Bad Request", "请求体不是 JPEG");
        if (flash) {
            const int closed = std::fclose(file.handle);
            file.handle = nullptr;
            if (closed != 0)
                return fail(req, "507 Insufficient Storage",
                            "Flash 缓存关闭失败");
        }
        BarcodeScanResult result;
        if (BarcodeWorker::scan(bytes.get(), received,
                                flash ? CACHE_PATH : nullptr,
                                static_cast<uint32_t>(id), result) != ESP_OK)
            return fail(req, "503 Service Unavailable",
                        "解码任务不可用或队列已满");
        if (result.json.empty())
            return fail(req, "503 Service Unavailable", "解码结果内存不足");
        const char *status = result.httpStatus == 200   ? "200 OK"
                             : result.httpStatus == 400 ? "400 Bad Request"
                             : result.httpStatus == 503
                                 ? "503 Service Unavailable"
                                 : "422 Unprocessable Content";
        httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Connection", "close");
        return httpd_resp_send(req, result.json.c_str(), result.json.size());
    } catch (const std::exception &) {
        return fail(req, "503 Service Unavailable", "扫码服务内存不足");
    }
}
} // namespace

esp_err_t BarcodeScanServer::start() {
    const esp_err_t workerResult = BarcodeWorker::start();
    if (workerResult != ESP_OK)
        return workerResult;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32769;
    config.core_id = 0;
    config.stack_size = 8192;
    config.max_uri_handlers = 2;
    // 两个 HTTP 服务共享全局 socket 池，连接满时回收最久未使用的会话。
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.keep_alive_enable = false;
    config.backlog_conn = 4;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    httpd_handle_t server = nullptr;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK)
        return result;
    std::remove(CACHE_PATH);
    const httpd_uri_t page = {.uri = "/",
                              .method = HTTP_GET,
                              .handler = pageHandler,
                              .user_ctx = nullptr};
    const httpd_uri_t scan = {.uri = "/api/barcode/scan",
                              .method = HTTP_POST,
                              .handler = scanHandler,
                              .user_ctx = nullptr};
    result = httpd_register_uri_handler(server, &page);
    if (result == ESP_OK)
        result = httpd_register_uri_handler(server, &scan);
    if (result != ESP_OK)
        httpd_stop(server);
    else
        ESP_LOGI(TAG, "扫码服务已启动，端口 81，最大输入 4096×4096 / 1 MiB");
    return result;
}

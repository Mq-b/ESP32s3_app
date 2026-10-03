#include "web_assets.h"

#include "esp_log.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>

namespace {

constexpr const char *TAG = "WEB_ASSETS";
constexpr size_t CHUNK_SIZE = 1024;

}  // namespace

esp_err_t sendHtmlFile(httpd_req_t *req, const char *path) {
    using FilePtr = std::unique_ptr<FILE, decltype(&std::fclose)>;
    FilePtr file(std::fopen(path, "rb"), &std::fclose);
    if (!file) {
        const bool missing = errno == ENOENT;
        ESP_LOGW(TAG, "页面文件打开失败: %s", path);
        return httpd_resp_send_err(req, missing ? HTTPD_404_NOT_FOUND : HTTPD_500_INTERNAL_SERVER_ERROR,
                                   missing ? "页面文件不存在" : "页面文件不可用");
    }
    const esp_err_t typeResult = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (typeResult != ESP_OK) return typeResult;

    std::array<char, CHUNK_SIZE> buffer{};
    bool started = false;
    while (true) {
        const size_t size = std::fread(buffer.data(), 1, buffer.size(), file.get());
        if (std::ferror(file.get())) {
            ESP_LOGE(TAG, "页面文件读取失败: %s", path);
            // 已发出响应时不能改发 500，也不能发送正常结束块；返回失败以关闭连接。
            return started ? ESP_FAIL : httpd_resp_send_err(
                req, HTTPD_500_INTERNAL_SERVER_ERROR, "页面文件读取失败");
        }
        if (size == 0) break;
        const esp_err_t result = httpd_resp_send_chunk(req, buffer.data(), size);
        if (result != ESP_OK) return result;
        started = true;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

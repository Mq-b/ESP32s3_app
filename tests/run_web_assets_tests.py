"""使用真实文件和最小 HTTP 桩验证 HTML 分块发送，不访问开发板。"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from run_app_config_tests import STUBS as CONFIG_STUBS


HTTP_STUB = """#pragma once
#include "esp_err.h"
#include <algorithm>
#include <cstddef>
#include <string>
constexpr int HTTPD_404_NOT_FOUND = 404;
constexpr int HTTPD_500_INTERNAL_SERVER_ERROR = 500;
/** @brief 记录响应内容和模拟发送失败的 HTTP 请求桩。 */
struct httpd_req_t {
    std::string type;
    std::string body;
    int errorStatus = -1;
    std::size_t chunks = 0;
    std::size_t maxChunk = 0;
    bool ended = false;
    bool failType = false;
    int failChunk = -1;
    bool failEnd = false;
};
/** @brief 设置测试响应类型，可模拟失败。 */
inline int httpd_resp_set_type(httpd_req_t *req, const char *type) {
    if (req->failType) return ESP_FAIL;
    req->type = type;
    return ESP_OK;
}
/** @brief 记录错误响应，500 时模拟连接关闭并返回失败。 */
inline int httpd_resp_send_err(httpd_req_t *req, int status, const char *) {
    req->errorStatus = status;
    return status == HTTPD_500_INTERNAL_SERVER_ERROR ? ESP_FAIL : ESP_OK;
}
/** @brief 记录分块响应，支持模拟数据块或结束块发送失败。 */
inline int httpd_resp_send_chunk(httpd_req_t *req, const char *data, std::ptrdiff_t size) {
    if (size == 0) {
        if (req->failEnd) return ESP_FAIL;
        req->ended = true;
        return ESP_OK;
    }
    if (static_cast<int>(req->chunks) == req->failChunk) return ESP_FAIL;
    ++req->chunks;
    req->maxChunk = std::max(req->maxChunk, static_cast<std::size_t>(size));
    req->body.append(data, static_cast<std::size_t>(size));
    return ESP_OK;
}
"""


def main():
    """编译真实文件发送模块并验证正常及失败路径，产物在临时目录清理。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="web-assets-tests-") as directory:
        temporary = Path(directory)
        for name in ("esp_err.h", "esp_log.h"):
            (temporary / name).write_text(CONFIG_STUBS[name], encoding="utf-8")
        (temporary / "esp_http_server.h").write_text(HTTP_STUB, encoding="utf-8")
        empty = temporary / "empty.html"
        empty.write_bytes(b"")
        executable = temporary / ("test_web_assets.exe" if os.name == "nt" else "test_web_assets")
        subprocess.run([args.cxx, "-std=c++23", "-fno-exceptions", "-Wall", "-Wextra", "-Werror",
                        "-I", str(temporary), "-I", str(root / "main"),
                        str(root / "main" / "web_assets.cpp"), str(root / "tests" / "test_web_assets.cpp"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable), str(root / "data" / "index.html"), str(empty),
                        str(temporary / "missing.html"), str(temporary)], check=True)


if __name__ == "__main__":
    main()

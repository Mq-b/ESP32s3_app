#include "web_assets.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

/** @brief 校验文件分块发送的回归断言，失败时退出测试。 */
void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

/**
 * @brief 验证真实 HTML 文件发送实现的内容、分块边界和失败处理。
 * @param argc 命令行参数数量，必须为 5。
 * @param argv 非空页面、空文件、缺失文件以及目录的测试路径。
 * @return 测试成功时返回 0，否则退出并返回非零状态。
 */
int main(int argc, char **argv) {
    check(argc == 5, "测试文件参数数量错误");
    std::ifstream input(argv[1], std::ios::binary);
    const std::string expected((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    check(expected.size() > 2048, "页面测试文件不足以覆盖多块发送");

    httpd_req_t normal;
    check(sendHtmlFile(&normal, argv[1]) == ESP_OK, "正常页面发送失败");
    check(normal.type == "text/html; charset=utf-8", "页面类型错误");
    check(normal.body == expected, "发送的页面内容不完整或改变");
    check(normal.chunks == (expected.size() + 1023) / 1024 && normal.maxChunk <= 1024,
          "页面未按 1024 字节分块发送");
    check(normal.ended && normal.errorStatus == -1, "正常页面未发送结束块");

    httpd_req_t empty;
    check(sendHtmlFile(&empty, argv[2]) == ESP_OK && empty.body.empty() && empty.ended,
          "空文件未正常结束");
    httpd_req_t missing;
    check(sendHtmlFile(&missing, argv[3]) == ESP_OK && missing.errorStatus == HTTPD_404_NOT_FOUND &&
          missing.chunks == 0, "页面缺失未返回 404");
    httpd_req_t unavailable;
    check(sendHtmlFile(&unavailable, argv[4]) == ESP_FAIL &&
          unavailable.errorStatus == HTTPD_500_INTERNAL_SERVER_ERROR, "不可读文件未返回 500");

    httpd_req_t typeFailure;
    typeFailure.failType = true;
    check(sendHtmlFile(&typeFailure, argv[1]) == ESP_FAIL && typeFailure.chunks == 0,
          "设置响应类型失败后仍发送文件");
    for (int failedChunk : {0, 1}) {
        httpd_req_t failed;
        failed.failChunk = failedChunk;
        check(sendHtmlFile(&failed, argv[1]) == ESP_FAIL, "发送失败未返回错误");
        check(!failed.ended && failed.errorStatus == -1, "发送失败后仍正常结束或重发错误响应");
    }
    httpd_req_t endFailure;
    endFailure.failEnd = true;
    check(sendHtmlFile(&endFailure, argv[1]) == ESP_FAIL && endFailure.body == expected && !endFailure.ended,
          "结束块发送失败未返回错误");
    std::cout << "HTML 文件分块发送回归测试通过\n";
}

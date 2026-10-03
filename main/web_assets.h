#pragma once

#include "esp_http_server.h"

/**
 * @brief 分块读取并发送 UTF-8 HTML 文件，文件不存在时返回 HTTP 404。
 * @param req 当前 HTTP 请求，不得为空。
 * @param path 以空字符结尾的文件路径，不得为空。
 * @return HTTP 响应发送结果；开始发送后的文件读取失败返回 ESP_FAIL。
 * @note 文件系统须已挂载，仅允许传入受信任路径，不可直接使用客户端输入。
 */
esp_err_t sendHtmlFile(httpd_req_t *req, const char *path);

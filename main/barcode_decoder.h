#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

/** @brief 单帧扫码结果，JSON 包含内容、四角和各阶段耗时。 */
struct BarcodeScanResult {
    int httpStatus = 200; /**< HTTP 状态码，资源不足或输入错误时为非 200。 */
    std::string json;     /**< UTF-8 JSON，非法文本使用替换字符安全序列化。 */
};

/**
 * @brief 解码有界 JPEG 并识别条码，不保存图像或执行网络操作。
 * @note 调用方须串行使用；原图单边最大 4096
 * 像素，保留原始分辨率识别，输入缓冲在返回前须有效。
 */
class BarcodeDecoder {
public:
    /**
     * @brief 从内存或临时文件解码一帧 JPEG。
     * @param data JPEG 字节，文件模式时为空。
     * @param size JPEG 字节数，最大 1 MiB。
     * @param path JPEG 文件路径，内存模式时为空。
     * @param frameId 浏览器帧编号，原样返回用于过滤迟到结果。
     * @return JSON 扫码结果及 HTTP 状态码；尺寸与四角均使用原图坐标。
     */
    static BarcodeScanResult scan(const uint8_t *data, size_t size,
                                  const char *path, uint32_t frameId);
};

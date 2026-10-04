#pragma once
#include "barcode_decoder.h"
#include "esp_err.h"

/** @brief 固定运行在 CPU 1 的串行解码任务，生命周期覆盖整个应用。 */
class BarcodeWorker {
public:
    /** @brief 创建长度为 1 的交接队列和解码任务，须在启动 HTTP 前调用。
     * @return ESP_OK 表示就绪，ESP_ERR_NO_MEM 表示资源创建失败。
     * @note 仅从初始化任务串行调用；重复调用不重复创建任务。
     */
    static esp_err_t start();
    /** @brief 交接一帧并阻塞等待结果，等待期间 HTTP 任务不占 CPU。
     * @param data 内存 JPEG，文件模式可为空。
     * @param size JPEG 字节数。
     * @param path 文件路径，内存模式为空。
     * @param frameId 帧编号。
     * @param result 返回的解码结果。
     * @return 任务不可用或交接失败返回错误，否则 ESP_OK。
     * @note 调用方须串行使用，输入在返回前保持有效；不提前超时释放输入。
     */
    static esp_err_t scan(const uint8_t *data, size_t size, const char *path,
                          uint32_t frameId, BarcodeScanResult &result);
};

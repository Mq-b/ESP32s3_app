#include "barcode_worker.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
QueueHandle_t jobs = nullptr;

/** @brief 一帧同步交接参数，调用方等到完成后才释放输入和结果。 */
struct DecodeJob {
    const uint8_t *data;
    size_t size;
    const char *path;
    uint32_t frameId;
    BarcodeScanResult *result;
    SemaphoreHandle_t completed;
};

/** @brief 串行处理队列中的图像，空闲时阻塞，不重复创建或删除任务。 */
void workerEntry(void *) {
    DecodeJob *job = nullptr;
    for (;;) {
        if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE)
            continue;
        try {
            *job->result = BarcodeDecoder::scan(job->data, job->size, job->path,
                                                job->frameId);
        } catch (...) {
            // 连错误 JSON 都无法分配时仍必须唤醒 HTTP，避免永久等待。
            job->result->httpStatus = 503;
            job->result->json.clear();
        }
        // 发出完成信号后不再访问调用方栈上的 job。
        const SemaphoreHandle_t completed = job->completed;
        xSemaphoreGive(completed);
    }
}
} // namespace

esp_err_t BarcodeWorker::start() {
    if (jobs)
        return ESP_OK;
    jobs = xQueueCreate(1, sizeof(DecodeJob *));
    if (!jobs)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(workerEntry, "barcode_decode", 24 * 1024,
                                nullptr, 2, nullptr, 1) != pdPASS) {
        vQueueDelete(jobs);
        jobs = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI("BARCODE_WORKER", "解码任务已启动：CPU 1，优先级 2，队列长度 1");
    return ESP_OK;
}

esp_err_t BarcodeWorker::scan(const uint8_t *data, size_t size,
                              const char *path, uint32_t frameId,
                              BarcodeScanResult &result) {
    if (!jobs)
        return ESP_ERR_INVALID_STATE;
    SemaphoreHandle_t completed = xSemaphoreCreateBinary();
    if (!completed)
        return ESP_ERR_NO_MEM;
    DecodeJob job{data, size, path, frameId, &result, completed};
    DecodeJob *pointer = &job;
    const BaseType_t sent = xQueueSend(jobs, &pointer, 0);
    if (sent == pdTRUE)
        xSemaphoreTake(completed, portMAX_DELAY);
    vSemaphoreDelete(completed);
    return sent == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

#include "system_monitor.h"

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdio>

namespace {

/**
 * @brief 读取指定能力的堆内存统计。
 * @param caps ESP-IDF 内存能力掩码。
 * @return 包含可分配总量、空闲量、历史最低空闲量和最大连续块的 JSON。
 */
std::string heapJson(uint32_t caps) {
    multi_heap_info_t info{};
    heap_caps_get_info(&info, caps);
    return "{\"total_bytes\":" + std::to_string(info.total_free_bytes + info.total_allocated_bytes) +
           ",\"free_bytes\":" + std::to_string(info.total_free_bytes) +
           ",\"minimum_free_bytes\":" + std::to_string(info.minimum_free_bytes) +
           ",\"largest_free_block_bytes\":" + std::to_string(info.largest_free_block) + "}";
}

/**
 * @brief 将 CPU 使用率转换为保留一位小数的 JSON 数值。
 * @param value 使用率百分比，范围为 0 至 100。
 * @return 数值字符串。
 */
std::string percentJson(double value) {
    char text[16]{};
    snprintf(text, sizeof(text), "%.1f", value);
    return text;
}

}  // namespace

std::string SystemMonitor::statusJson() {
    const int64_t nowUs = esp_timer_get_time();
#if configGENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    // 使用 64 位微秒计数；仅更新足够长的采样窗口，避免多浏览器缩短采样周期。
    if (previousTimeUs_ == 0 || nowUs - previousTimeUs_ >= 1000000) {
        std::array<uint64_t, portNUM_PROCESSORS> idle{};
        for (size_t core = 0; core < idle.size(); ++core) {
            idle[core] = ulTaskGetIdleRunTimeCounterForCore(core);
        }
        if (previousTimeUs_ != 0) {
            const auto elapsedUs = static_cast<uint64_t>(nowUs - previousTimeUs_);
            for (size_t core = 0; core < idle.size(); ++core) {
                const uint64_t idleDelta = idle[core] - previousIdle_[core];
                usage_[core] = 100.0 * (1.0 - static_cast<double>(std::min(idleDelta, elapsedUs)) / elapsedUs);
            }
            sampleWindowMs_ = elapsedUs / 1000;
            cpuReady_ = true;
        }
        previousIdle_ = idle;
        previousTimeUs_ = nowUs;
    }
#endif

    std::string cores = "[";
    double average = 0;
    for (size_t core = 0; core < usage_.size(); ++core) {
        if (core != 0) cores += ',';
        cores += cpuReady_ ? percentJson(usage_[core]) : "null";
        average += usage_[core];
    }
    cores += ']';

    size_t storageTotal = 0;
    size_t storageUsed = 0;
    const bool storageAvailable = esp_spiffs_info("storage", &storageTotal, &storageUsed) == ESP_OK;
    uint32_t flashBytes = 0;
    const bool flashAvailable = esp_flash_get_size(nullptr, &flashBytes) == ESP_OK;

    return "{\"uptime_ms\":" + std::to_string(nowUs / 1000) +
           ",\"task_count\":" + std::to_string(uxTaskGetNumberOfTasks()) +
           ",\"memory\":{\"internal\":" + heapJson(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) +
           ",\"psram\":" + heapJson(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) +
           "},\"storage\":{\"available\":" + (storageAvailable ? "true" : "false") +
           ",\"total_bytes\":" + std::to_string(storageTotal) +
           ",\"used_bytes\":" + std::to_string(storageUsed) +
           "},\"flash_bytes\":" + (flashAvailable ? std::to_string(flashBytes) : "null") +
           ",\"cpu\":{\"available\":" + (cpuReady_ ? "true" : "false") +
           ",\"sample_window_ms\":" + std::to_string(sampleWindowMs_) +
           ",\"sample_age_ms\":" + std::to_string(cpuReady_ ? (nowUs - previousTimeUs_) / 1000 : 0) +
           ",\"usage_percent\":" + (cpuReady_ ? percentJson(average / usage_.size()) : "null") +
           ",\"cores_percent\":" + cores + "}}";
}

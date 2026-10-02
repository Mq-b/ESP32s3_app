#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "freertos/FreeRTOS.h"

/**
 * @brief 系统资源采集器，按 HTTP 请求采集内存、存储和 CPU 调度负载。
 * @note 仅由 HTTP 服务任务串行调用；不创建后台任务，不执行阻塞式采样。
 */
class SystemMonitor {
public:
    /**
     * @brief 获取系统资源 JSON，并在至少间隔一秒时更新 CPU 采样。
     * @return UTF-8 JSON；CPU 首次采样或统计未启用时，负载字段为 null。
     * @note CPU 负载基于空闲任务累计时间差，属于估算值；不等同于性能剖析。
     */
    std::string statusJson();

private:
    int64_t previousTimeUs_ = 0;
    std::array<uint64_t, portNUM_PROCESSORS> previousIdle_{};
    std::array<double, portNUM_PROCESSORS> usage_{};
    uint64_t sampleWindowMs_ = 0;
    bool cpuReady_ = false;
};

#include "esp_log.h"
#include "esp_timer.h"

#include <cstdint>
#include <string>
#include <vector>

static const char *TAG = "APP";

/**
 * @brief ESP32 运算性能测试，执行 count 次整数与浮点混合运算
 * @param count 循环次数，越大耗时越长（建议 1000000 起步）
 * @return 结果校验值，供打印或比对，同时防止循环被优化
 * @note acc 用 volatile 修饰，避免编译器把整个循环优化掉
 */
uint32_t perfTest(uint32_t count) {
    volatile uint32_t acc = 1;
    for (uint32_t i = 1; i <= count; i++) {
        acc += i * 3u;                           /* 整数乘加 */
        acc ^= (acc >> 7);                       /* 位移异或 */
        acc += (uint32_t)((float)acc * 1.0001f); /* 浮点乘 */
    }
    return acc;
}

/**
 * @brief 应用入口，多轮跑分并用 STL 汇总输出
 */
extern "C" void app_main(void) {
    ESP_LOGI(TAG, "ESP32-S3 启动成功");

    std::vector<uint32_t> results; /* 每轮结果 */
    std::vector<uint32_t> times;   /* 每轮耗时(us) */
    results.reserve(3);
    times.reserve(3);

    for (int i = 0; i < 3; i++) {
        uint32_t t0 = esp_timer_get_time();
        uint32_t r = perfTest(1000000);
        uint32_t dt = esp_timer_get_time() - t0;
        results.push_back(r);
        times.push_back(dt);
    }

    /* 用 std::string 拼日志 */
    std::string msg = "性能测试:";
    for (size_t i = 0; i < results.size(); i++) {
        msg += " [第" + std::to_string(i + 1) +
               "轮 结果=" + std::to_string(results[i]) +
               " 耗时=" + std::to_string(times[i]) + "us]";
    }
    ESP_LOGI(TAG, "%s", msg.c_str());
}
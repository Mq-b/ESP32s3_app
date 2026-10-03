#include "system_monitor.cpp"

#include <cstdlib>
#include <iostream>

namespace {

/** @brief 校验系统 JSON 回归断言，失败时退出测试。 */
void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

/** @brief 在启用及禁用 CPU 统计时验证真实系统状态序列化。 */
int main() {
    SystemMonitor monitor;
    Json status = Json::parse(monitor.statusJson());
    check(status.is_object() && status.size() == 6, "系统响应字段数量改变");
    check(status["uptime_ms"] == 1000 && status["task_count"] == 12, "时间或任务数错误");
    check(status["flash_bytes"] == 8388608, "Flash 大小错误");
    check(status["memory"]["internal"]["total_bytes"] == 200000, "内存统计错误");
    check(status["memory"]["internal"]["free_bytes"] == 100000, "空闲内存错误");
    check(status["memory"]["internal"]["minimum_free_bytes"] == 80000, "最低空闲内存错误");
    check(status["memory"]["internal"]["largest_free_block_bytes"] == 50000, "最大连续块错误");
    check(status["memory"]["psram"]["total_bytes"] == 0, "未启用 PSRAM 未返回零");
    check(status["storage"]["available"] == true, "存储状态不是布尔值");
    check(status["storage"]["total_bytes"] == 1000000 &&
          status["storage"]["used_bytes"] == 100000, "存储容量错误");
    check(status["cpu"]["available"] == false && status["cpu"]["usage_percent"].is_null(),
          "首次 CPU 负载未返回 null");
    check(status["cpu"]["cores_percent"].is_array() && status["cpu"]["cores_percent"].size() == 2,
          "CPU 核心字段不是双核数组");
    for (const Json &value : status["cpu"]["cores_percent"]) check(value.is_null(), "首次核心负载未返回 null");

    test_monitor::nowUs = 2000000;
    test_monitor::idle = {500000, 800000};
    status = Json::parse(monitor.statusJson());
#if configGENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
    check(status["cpu"]["available"] == true, "第二次 CPU 采样未就绪");
    check(status["cpu"]["cores_percent"][0] == 50.0 && status["cpu"]["cores_percent"][1] == 20.0,
          "核心负载数值改变");
    check(status["cpu"]["usage_percent"] == 35.0, "平均 CPU 负载错误");
    check(status["cpu"]["sample_window_ms"] == 1000, "采样窗口错误");
    test_monitor::nowUs += 200000;
    status = Json::parse(monitor.statusJson());
    check(status["cpu"]["sample_age_ms"] == 200 && status["cpu"]["usage_percent"] == 35.0,
          "过早刷新更新了采样窗口");
    test_monitor::nowUs = 3000000;
    test_monitor::idle = {1166666, 1133333};
    status = Json::parse(monitor.statusJson());
    check(status["cpu"]["cores_percent"][0] == 33.3 && status["cpu"]["cores_percent"][1] == 66.7,
          "CPU 百分比未保留一位小数精度");
#else
    check(status["cpu"]["available"] == false && status["cpu"]["usage_percent"].is_null(),
          "禁用 CPU 统计后仍返回负载");
#endif
    test_monitor::storageAvailable = false;
    test_monitor::flashAvailable = false;
    status = Json::parse(monitor.statusJson());
    check(status["storage"]["available"] == false && status["storage"]["total_bytes"] == 0 &&
          status["storage"]["used_bytes"] == 0, "未挂载存储输出错误");
    check(status["flash_bytes"].is_null(), "Flash 读取失败未返回 null");
    std::cout << "系统状态 JSON 回归测试通过\n";
}

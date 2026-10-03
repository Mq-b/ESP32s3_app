"""使用最小 ESP-IDF 桩验证系统 JSON 响应，分别启用和禁用 CPU 统计。"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from run_app_config_tests import STUBS as CONFIG_STUBS


STUBS = {
    "esp_err.h": CONFIG_STUBS["esp_err.h"],
    "monitor_test_state.h": """#pragma once
#include <array>
#include <cstdint>
namespace test_monitor {
inline int64_t nowUs = 1000000;
inline std::array<uint64_t, 2> idle{};
inline bool storageAvailable = true;
inline bool flashAvailable = true;
}
""",
    "freertos/FreeRTOS.h": """#pragma once
#define portNUM_PROCESSORS 2
""",
    "freertos/task.h": """#pragma once
#include "monitor_test_state.h"
/** @brief 返回测试任务数量。 */
inline unsigned uxTaskGetNumberOfTasks() { return 12; }
/** @brief 返回指定核心的模拟空闲任务累计时间。 */
inline uint64_t ulTaskGetIdleRunTimeCounterForCore(unsigned core) { return test_monitor::idle[core]; }
""",
    "esp_timer.h": """#pragma once
#include "monitor_test_state.h"
/** @brief 返回可控的模拟时间。 */
inline int64_t esp_timer_get_time() { return test_monitor::nowUs; }
""",
    "esp_flash.h": """#pragma once
#include "esp_err.h"
#include "monitor_test_state.h"
/** @brief 模拟成功或失败的 Flash 大小查询。 */
inline int esp_flash_get_size(void *, uint32_t *size) {
    if (!test_monitor::flashAvailable) return ESP_FAIL;
    *size = 8388608;
    return ESP_OK;
}
""",
    "esp_spiffs.h": """#pragma once
#include "esp_err.h"
#include "monitor_test_state.h"
#include <cstddef>
/** @brief 模拟挂载或未挂载的存储统计。 */
inline int esp_spiffs_info(const char *, std::size_t *total, std::size_t *used) {
    if (!test_monitor::storageAvailable) return ESP_FAIL;
    *total = 1000000;
    *used = 100000;
    return ESP_OK;
}
""",
    "esp_heap_caps.h": """#pragma once
#include <cstddef>
#include <cstdint>
constexpr uint32_t MALLOC_CAP_INTERNAL = 1;
constexpr uint32_t MALLOC_CAP_8BIT = 2;
constexpr uint32_t MALLOC_CAP_SPIRAM = 4;
/** @brief 堆内存统计测试桩。 */
struct multi_heap_info_t {
    std::size_t total_free_bytes;
    std::size_t total_allocated_bytes;
    std::size_t minimum_free_bytes;
    std::size_t largest_free_block;
};
/** @brief 返回模拟内部内存统计；PSRAM 未启用。 */
inline void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps) {
    *info = caps & MALLOC_CAP_INTERNAL ? multi_heap_info_t{100000, 100000, 80000, 50000}
                                     : multi_heap_info_t{};
}
""",
}


def main():
    """编译真实系统监控实现，测试产物在临时目录自动清理。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    component = root / "managed_components" / "mittelab__nlohmann-json"
    if not (component / "nlohmann" / "json_impl.hpp").exists():
        parser.error("请先配置 ESP-IDF 工程以下载 JSON 组件")
    with tempfile.TemporaryDirectory(prefix="system-monitor-tests-") as directory:
        temporary = Path(directory)
        for name, content in STUBS.items():
            path = temporary / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        for enabled in (0, 1):
            executable = temporary / (f"test_system_monitor_{enabled}" + (".exe" if os.name == "nt" else ""))
            subprocess.run([args.cxx, "-std=c++23", "-fno-exceptions", "-Wall", "-Wextra", "-Werror",
                            f"-DconfigGENERATE_RUN_TIME_STATS={enabled}",
                            f"-DCONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER={enabled}",
                            "-I", str(temporary), "-I", str(root / "main"), "-I", str(component),
                            str(root / "tests" / "test_system_monitor.cpp"), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

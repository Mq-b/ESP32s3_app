"""使用真实 ZXing、JPEG 解码器和示例图片验证固件扫码，不模拟识别结果。"""

import os
from pathlib import Path
import subprocess
import textwrap


def main():
    """构建主机回归程序，验证两张示例、内存/文件输入及优先级恢复。"""
    root = Path(__file__).resolve().parents[1]
    work = root / "build" / "barcode-host-tests"
    work.mkdir(parents=True, exist_ok=True)
    stubs = {
        "esp_heap_caps.h": """#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_realloc(void *p, size_t n, int caps) { (void)caps; return realloc(p, n); }
static inline void heap_caps_free(void *p) { free(p); }
""",
        "esp_timer.h": """#pragma once
#include <chrono>
inline long long esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
""",
        "esp_log.h": """#pragma once
#include <cstdio>
#define ESP_LOGI(tag, ...) do { (void)(tag); std::printf(__VA_ARGS__); std::puts(""); } while (0)
""",
        "freertos/FreeRTOS.h": """#pragma once
using UBaseType_t = unsigned;
constexpr UBaseType_t tskIDLE_PRIORITY = 0;
using BaseType_t = int;
using TickType_t = unsigned;
constexpr BaseType_t pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = ~0U;
inline thread_local UBaseType_t testPriority = 5;
inline unsigned testDelayCount = 0;
inline int testWorkerCore = -1;
inline unsigned testWorkerPriority = 0;
""",
        "freertos/task.h": """#pragma once
#include "FreeRTOS.h"
inline UBaseType_t uxTaskPriorityGet(void *) { return testPriority; }
inline void vTaskPrioritySet(void *, UBaseType_t p) { testPriority = p; }
using TaskHandle_t = unsigned *;
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return &testPriority; }
inline int xPortGetCoreID() { return 1; }
#include <thread>
#include <chrono>
inline void vTaskDelay(unsigned ticks) { ++testDelayCount; std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
inline BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *, unsigned,
        void *arg, unsigned priority, void *, int core) {
    testWorkerCore = core;
    testWorkerPriority = priority;
    std::thread([=] { testPriority = priority; entry(arg); }).detach();
    return pdPASS;
}
""",
    }
    stubs["esp_err.h"] = """#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NO_MEM = 1, ESP_ERR_INVALID_STATE = 2;
"""
    stubs["freertos/queue.h"] = """#pragma once
#include "FreeRTOS.h"
#include <mutex>
#include <condition_variable>
#include <deque>
struct TestQueue { std::mutex mutex; std::condition_variable cv; std::deque<void *> entries; unsigned capacity; };
using QueueHandle_t = TestQueue *;
inline QueueHandle_t xQueueCreate(unsigned capacity, unsigned) { return new TestQueue{{}, {}, {}, capacity}; }
inline void vQueueDelete(QueueHandle_t q) { delete q; }
inline BaseType_t xQueueSend(QueueHandle_t q, void *item, TickType_t) {
    std::lock_guard lock(q->mutex);
    if (q->entries.size() >= q->capacity) return 0;
    q->entries.push_back(*static_cast<void **>(item)); q->cv.notify_one(); return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t) {
    std::unique_lock lock(q->mutex); q->cv.wait(lock, [&] { return !q->entries.empty(); });
    *static_cast<void **>(item) = q->entries.front(); q->entries.pop_front(); return pdTRUE;
}
"""
    stubs["freertos/semphr.h"] = """#pragma once
#include "FreeRTOS.h"
#include <mutex>
#include <condition_variable>
struct TestSemaphore { std::mutex mutex; std::condition_variable cv; bool ready = false; };
using SemaphoreHandle_t = TestSemaphore *;
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return new TestSemaphore; }
inline void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    std::lock_guard lock(s->mutex); s->ready = true; s->cv.notify_one(); return pdTRUE;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t) {
    std::unique_lock lock(s->mutex); s->cv.wait(lock, [&] { return s->ready; }); return pdTRUE;
}
"""
    for name, source in stubs.items():
        target = work / "stubs" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source, encoding="utf-8")
    (work / "test.cpp").write_text(r'''#include "barcode_decoder.h"
#include "barcode_worker.h"
#include "freertos/FreeRTOS.h"
#include "stb_image.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <cstdlib>
/** @brief 检查真实解码结果，失败立即退出。 */
void check(bool ok, const char *message) {
    if (!ok) { std::cerr << message << "\n"; std::exit(1); }
}
/** @brief 校验两张项目样例的内存与文件模式，及非法输入。 */
int main(int argc, char **argv) {
    check(argc == 3, "必须提供两张样例图片");
    BarcodeScanResult unavailable;
    check(BarcodeWorker::scan(nullptr, 0, nullptr, 0, unavailable) == ESP_ERR_INVALID_STATE, "未启动任务未拒绝");
    check(BarcodeWorker::start() == ESP_OK && BarcodeWorker::start() == ESP_OK, "任务启动或重复启动失败");
    check(testWorkerCore == 1 && testWorkerPriority == 2, "任务核亲和性或优先级错误");
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        check(!bytes.empty(), "图片读取失败");
        int width = 0, height = 0, channels = 0;
        check(stbi_info_from_memory(bytes.data(), bytes.size(), &width, &height, &channels), "样例不是 JPEG");
        for (bool file : {false, true}) {
            BarcodeScanResult result;
            check(BarcodeWorker::scan(file ? nullptr : bytes.data(), bytes.size(), file ? argv[i] : nullptr, 17, result) == ESP_OK,
                  "任务交接失败");
            auto json = nlohmann::json::parse(result.json);
            check(result.httpStatus == 200 && json["ok"] == true, "扫码返回错误");
            check(json["frameId"] == 17 && json["width"] == width && json["height"] == height, "原图尺寸或帧编号改变");
            check(!json["codes"].empty(), "真实样例未识别到条码");
            check(testPriority == 5, "解码后未恢复任务优先级");
            for (auto &code : json["codes"]) {
                check(code["format"] == "DataMatrix" && code["text"] == "G99367R01A", "样例格式或内容错误");
                check(code["corners"].size() == 4, "坐标数量错误");
                for (auto &point : code["corners"])
                    check(point["x"] >= 0 && point["x"] <= width && point["y"] >= 0 && point["y"] <= height, "坐标超出原图");
            }
            std::cout << result.json << "\n";
        }
    }
    check(BarcodeDecoder::scan(nullptr, 0, nullptr, 18).httpStatus == 400, "空输入未拒绝");
    check(testPriority == 5, "错误返回改变任务优先级");
    std::cout << "真实扫码回归测试通过\n";
}
''', encoding="utf-8")
    (work / "runtime_test.cpp").write_text(r'''#include "barcode_runtime.h"
#include "freertos/task.h"
#include <cstdio>
/** @brief 验证计时预算和仅当前任务让出，不验证真实设备调度。 */
int main() {
    barcode_runtime_checkpoint();
    if (testDelayCount != 0) return 1;
    barcode_runtime_begin();
    barcode_runtime_checkpoint();
    if (testDelayCount != 0) return 2;
    std::this_thread::sleep_for(std::chrono::milliseconds(65));
    std::thread other([] { barcode_runtime_checkpoint(); });
    other.join();
    if (testDelayCount != 0) return 3;
    barcode_runtime_checkpoint();
    if (testDelayCount != 1) return 4;
    barcode_runtime_checkpoint();
    if (testDelayCount != 1) return 5;
    barcode_runtime_end();
    std::this_thread::sleep_for(std::chrono::milliseconds(65));
    barcode_runtime_checkpoint();
    if (testDelayCount != 1 || testPriority != 5) return 6;
    std::puts("协作调度预算回归测试通过");
}
''', encoding="utf-8")
    root_path = root.as_posix()
    (work / "CMakeLists.txt").write_text(textwrap.dedent(f"""
        cmake_minimum_required(VERSION 3.16)
        project(barcode_regression LANGUAGES C CXX)
        set(CMAKE_CXX_STANDARD 23)
        set(ZXING_READERS ON)
        set(ZXING_WRITERS OFF)
        set(BUILD_SHARED_LIBS OFF)
        add_subdirectory("{root_path}/components/zxing/upstream/core" zxing)
        add_executable(barcode_test test.cpp "{root_path}/main/barcode_decoder.cpp" "{root_path}/main/barcode_worker.cpp"
                       "{root_path}/components/stb_image/stb_image_impl.c")
        target_include_directories(barcode_test PRIVATE stubs "{root_path}/main"
            "{root_path}/components/stb_image/include" "{root_path}/components/barcode_runtime/include" "{root_path}/managed_components/mittelab__nlohmann-json")
        target_link_libraries(barcode_test PRIVATE ZXing)
        set(runtime_source "{root_path}/components/barcode_runtime/barcode_runtime.c")
        set_source_files_properties(${{runtime_source}} PROPERTIES LANGUAGE CXX)
        add_executable(runtime_test runtime_test.cpp ${{runtime_source}})
        target_compile_definitions(runtime_test PRIVATE ESP_PLATFORM)
        target_include_directories(runtime_test PRIVATE stubs "{root_path}/components/barcode_runtime/include")
    """), encoding="utf-8")
    subprocess.run(["cmake", "-S", str(work), "-B", str(work / "out"), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++"], check=True)
    subprocess.run(["cmake", "--build", str(work / "out"), "--parallel", "4"], check=True)
    executable = work / "out" / ("barcode_test.exe" if os.name == "nt" else "barcode_test")
    runtime_executable = work / "out" / ("runtime_test.exe" if os.name == "nt" else "runtime_test")
    subprocess.run([str(runtime_executable)], check=True)
    # 第一个 .jpg 样例实际是 PNG；模拟浏览器转换 JPEG 的输入流程。
    from PIL import Image
    samples = []
    for name in ("esp32-s3-barcode-example1.jpg", "esp32-s3-barcode-example2.jpg"):
        target = work / name
        with Image.open(root / "images" / name) as image:
            image.convert("RGB").save(target, format="JPEG", quality=85)
        samples.append(str(target))
    subprocess.run([str(executable), *samples], check=True)


if __name__ == "__main__":
    main()

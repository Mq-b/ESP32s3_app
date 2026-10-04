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
inline UBaseType_t testPriority = 5;
""",
        "freertos/task.h": """#pragma once
#include "FreeRTOS.h"
inline UBaseType_t uxTaskPriorityGet(void *) { return testPriority; }
inline void vTaskPrioritySet(void *, UBaseType_t p) { testPriority = p; }
""",
    }
    for name, source in stubs.items():
        target = work / "stubs" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source, encoding="utf-8")
    (work / "test.cpp").write_text(r'''#include "barcode_decoder.h"
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
    for (int i = 1; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        check(!bytes.empty(), "图片读取失败");
        int width = 0, height = 0, channels = 0;
        check(stbi_info_from_memory(bytes.data(), bytes.size(), &width, &height, &channels), "样例不是 JPEG");
        for (bool file : {false, true}) {
            auto result = BarcodeDecoder::scan(file ? nullptr : bytes.data(), bytes.size(), file ? argv[i] : nullptr, 17);
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
    root_path = root.as_posix()
    (work / "CMakeLists.txt").write_text(textwrap.dedent(f"""
        cmake_minimum_required(VERSION 3.16)
        project(barcode_regression LANGUAGES C CXX)
        set(CMAKE_CXX_STANDARD 23)
        set(ZXING_READERS ON)
        set(ZXING_WRITERS OFF)
        set(BUILD_SHARED_LIBS OFF)
        add_subdirectory("{root_path}/components/zxing/upstream/core" zxing)
        add_executable(barcode_test test.cpp "{root_path}/main/barcode_decoder.cpp"
                       "{root_path}/components/stb_image/stb_image_impl.c")
        target_include_directories(barcode_test PRIVATE stubs "{root_path}/main"
            "{root_path}/components/stb_image/include" "{root_path}/managed_components/mittelab__nlohmann-json")
        target_link_libraries(barcode_test PRIVATE ZXing)
    """), encoding="utf-8")
    subprocess.run(["cmake", "-S", str(work), "-B", str(work / "out"), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++"], check=True)
    subprocess.run(["cmake", "--build", str(work / "out"), "--parallel", "4"], check=True)
    executable = work / "out" / ("barcode_test.exe" if os.name == "nt" else "barcode_test")
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

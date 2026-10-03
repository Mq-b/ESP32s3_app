"""使用主机 C++ 编译器及最小 ESP-IDF 桩运行真实固件配置代码，不访问设备。"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


STUBS = {
    "esp_err.h": """#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_NO_MEM = 0x101;
constexpr int ESP_ERR_INVALID_ARG = 0x102;
constexpr int ESP_ERR_INVALID_STATE = 0x103;
/** @brief 返回测试用错误说明。 */
inline const char *esp_err_to_name(int) { return "测试错误"; }
""",
    "esp_log.h": """#pragma once
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
""",
    "esp_spiffs.h": """#pragma once
#include "esp_err.h"
#include <cstddef>
/** @brief 主机测试使用的 SPIFFS 配置桩。 */
struct esp_vfs_spiffs_conf_t {
    const char *base_path;
    const char *partition_label;
    std::size_t max_files;
    bool format_if_mount_failed;
};
/** @brief 禁止主机测试挂载设备文件系统。 */
inline int esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *) { return ESP_FAIL; }
""",
    "nvs.h": """#pragma once
#include "esp_err.h"
#include <cstddef>
using nvs_handle_t = int;
constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;
/** @brief 禁止主机测试打开真实 NVS。 */
inline int nvs_open(const char *, int, nvs_handle_t *) { return ESP_FAIL; }
/** @brief 主机桩不提供持久化数据。 */
inline int nvs_get_str(int, const char *, char *, std::size_t *) { return ESP_FAIL; }
/** @brief 主机桩不写入持久化数据。 */
inline int nvs_set_str(int, const char *, const char *) { return ESP_FAIL; }
/** @brief 主机桩不提交持久化数据。 */
inline int nvs_commit(int) { return ESP_FAIL; }
/** @brief 关闭空的测试句柄。 */
inline void nvs_close(int) {}
""",
}


def main():
    """编译并运行回归测试；构建结果放在自动清理的临时目录。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"),
                        help="支持 C++23 的 GCC/Clang 主机编译器")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    component = root / "managed_components" / "mittelab__nlohmann-json"
    if not (component / "nlohmann" / "json_impl.hpp").exists():
        parser.error("请先配置 ESP-IDF 工程以下载 mittelab/nlohmann-json 组件")
    with tempfile.TemporaryDirectory(prefix="app-config-tests-") as directory:
        temporary = Path(directory)
        for name, content in STUBS.items():
            (temporary / name).write_text(content, encoding="utf-8")
        executable = temporary / ("test_app_config.exe" if os.name == "nt" else "test_app_config")
        subprocess.run([args.cxx, "-std=c++23", "-fno-exceptions", "-Wall", "-Wextra",
                        "-Werror", "-I", str(temporary), "-I", str(root / "main"),
                        "-I", str(component), str(root / "tests" / "test_app_config.cpp"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

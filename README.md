# ESP32-S3 最小工程

ESP-IDF v6.1，目标 ESP32-S3。`main/main.cpp` 为 C++20 应用入口，`app_main` 使用 ESP-IDF 所需的 C 链接约定。无需开发板即可编译固件。

## 直接使用 CMake

在项目目录中：

```powershell
cmake --preset esp32s3
cmake --build --preset esp32s3
```

固件输出到 `build/esp32s3/esp32s3_app.bin`。仓库中的 `CMakePresets.json` 定义 `esp32s3` 配置与构建预设；`CMakeUserPresets.json` 用于本机个人预设扩展，不纳入 Git。

## 使用 IDF 环境编译

PowerShell 中先激活本机 ESP-IDF 安装脚本，再运行 `idf.py build`。本机脚本位置为 `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`，示例：

```powershell
$env:PROCESSOR_ARCHITECTURE = 'AMD64'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py build
```

本机普通终端缺少 `PROCESSOR_ARCHITECTURE` 时，IDF 会报 `Support for platform 'Windows-' hasn't been added yet.`，因此在激活前补齐。固件输出到 `build/esp32s3_app.bin`。以上命令只编译，不烧录。

## VS Code ESP-IDF 插件

用 VS Code 打开本项目目录（不要打开其上级目录）。本机 `.vscode/settings.json` 指向已安装的 IDF 配置及 `esp32s3` CMake Preset，属于个人配置，已忽略。插件 v2.3.0 所需的 `idf.eimIdfJsonPath` 是 VS Code 用户级设置，应指向本机安装器生成的 `eim_idf.json`，也不纳入 Git。

## clangd

项目的 `.clangd` 使用 `build/esp32s3/compile_commands.json` 提供 ESP-IDF 的真实头文件和宏配置，并将 clangd 的语言解析标准设置为 C++23。先执行一次上述 CMake 构建以生成编译数据库。此设置只影响 clangd 的诊断和补全，实际固件仍按 C++20 编译；不要仅凭 clangd 通过就使用 C++23 特性。

> clangd 红色误报处理：`.vscode/settings.json` 为 clangd 配置了 ESP32-S3 的交叉编译器 `--query-driver`，让 clangd 使用 Xtensa 工具链的 C++ 标准库头文件，而不是 Windows 主机头文件。该文件包含本机路径，已被 `.gitignore` 忽略。修改后请执行 **命令面板 → clangd: Restart language server**。

# ESP32-S3 内网 Web 服务 Demo

ESP-IDF v6.1，目标 ESP32-S3。设备以 STA 模式连接指定 WiFi，使用**固定 IP**，并在 80 端口运行一个 HTTP 服务，浏览器访问 `http://<固定IP>/` 即可看到设备首页。WiFi 凭据通过 JSON 配置文件加载，不进 Git。

## 功能

- WiFi STA 连接，固定 IP（默认 `192.168.0.10`），断线自动重连
- HTTP 服务（`esp_http_server`，80 端口），`GET /` 返回设备首页
- 配置外置：`data/wifi.json`（SPIFFS 文件系统），与代码分离、已 gitignore

## 项目结构

```
main/
  main.cpp          入口：初始化 NVS → 加载配置 → 连 WiFi → 起 HTTP 服务
  app_config.*      挂载 SPIFFS，解析 /spiffs/wifi.json（极简 JSON 解析，无外部依赖）
  wifi_manager.*    STA 连接、固定 IP、事件处理与自动重连
  web_server.*      HTTP 服务与首页 HTML
data/
  wifi.json         真实凭据（已 gitignore，需自行创建）
  wifi.example.json 配置模板（复制为 wifi.json 并填写）
partitions.csv      分区表：factory 2MB + storage(SPIFFS) 1MB
```

## 首次使用：创建配置

复制模板并填写自己的网络参数：

```powershell
Copy-Item data/wifi.example.json data/wifi.json
```

```json
{
  "ssid": "your-ssid",
  "password": "your-password",
  "static_ip": "192.168.0.10",
  "gateway": "192.168.0.1",
  "netmask": "255.255.255.0"
}
```

编译时 `data/` 目录会被打包为 SPIFFS 镜像 `storage.bin`，随烧录一并写入 flash（见 `main/CMakeLists.txt` 的 `spiffs_create_partition_image`）。

## 构建

直接使用 CMake（推荐）：

```powershell
cmake --preset esp32s3
cmake --build --preset esp32s3
```

固件输出到 `build/esp32s3/esp32s3_app.bin`，SPIFFS 镜像为 `build/esp32s3/storage.bin`。

或使用 IDF 环境（注意指定构建目录为 preset 的 `build/esp32s3`）：

```powershell
$env:PROCESSOR_ARCHITECTURE = 'AMD64'
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py -B build/esp32s3 build
```

> 注意：Git Bash / MSYS 环境下 IDF 的 Python 依赖检查会报 `MSys/Mingw is not supported`，请用 PowerShell。

## 烧录与运行

```powershell
idf.py -B build/esp32s3 flash monitor
```

烧录包含 bootloader、分区表、`storage.bin`（wifi.json）、app 四部分。ESP32-S3 原生 USB 烧录后若停在 `waiting for download`，按一下 RST 复位即可。

启动日志依次出现 `配置加载完成` → `正在连接 WiFi` → `HTTP 服务已启动` 后，浏览器访问：

```
http://192.168.0.10/
```

## flash 分区布局（8MB）

![flash layout](./images/flash_layout.svg)

| 地址 | 分区 | 大小 | 用途 |
|---|---|---|---|
| 0x0 | bootloader | 32KB | 上电第一段代码 |
| 0x8000 | 分区表 | 4KB | partitions.csv 编译产物 |
| 0x9000 | nvs | 24KB | WiFi 校准等键值存储 |
| 0xF000 | phy_init | 4KB | 射频校准数据 |
| 0x10000 | factory | 2MB | 应用固件 |
| 0x210000 | storage | 1MB | SPIFFS，存放 wifi.json |
| 0x310000 起 | 未分配 | ~4.9MB | 预留扩展（OTA / 更多存储） |

## 硬件资源

- Flash（外置）：8MB（以 `esptool.py flash_id` 实测为准）
- SRAM（内置）：512KB 运行内存
- PSRAM：视模组型号（N8R2/N8R8 等），当前未启用

## VS Code ESP-IDF 插件

用 VS Code 打开本项目目录（不要打开其上级目录）。本机 `.vscode/settings.json` 指向已安装的 IDF 配置及 `esp32s3` CMake Preset，属于个人配置，已忽略。

## clangd

项目的 `.clangd` 使用 `build/esp32s3/compile_commands.json` 提供 ESP-IDF 的真实头文件和宏配置，并将 clangd 的语言解析标准设置为 C++23。先执行一次上述 CMake 构建以生成编译数据库。此设置只影响 clangd 的诊断和补全，实际固件仍按 C++20 编译；不要仅凭 clangd 通过就使用 C++23 特性。

> clangd 红色误报处理：`.vscode/settings.json` 为 clangd 配置了 ESP32-S3 的交叉编译器 `--query-driver`，让 clangd 使用 Xtensa 工具链的 C++ 标准库头文件，而不是 Windows 主机头文件。该文件包含本机路径，已被 `.gitignore` 忽略。修改后请执行 **命令面板 → clangd: Restart language server**。

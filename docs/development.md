# 项目结构、调试与排障

[返回项目首页](../README.md)

以下调试记录以当前开发环境为例，个人安装路径和工具版本需按实际环境调整。

## 项目结构

```
main/
  main.cpp          入口：初始化 NVS → 加载配置 → 启动 BLE / WiFi / HTTP 服务
  app_config.*      挂载 SPIFFS，校验 JSON，优先加载 NVS 配置并同步 /spiffs/wifi.json（cJSON）
  wifi_manager.*    STA 连接、固定 IP、事件处理与自动重连
  web_server.*      HTTP 服务与首页 HTML
  ble_config_service.* BLE 广播发现、认证配对与网络配置读写
tools/
  configure_wifi.pyw 单文件蓝牙配网 GUI
tests/
  test_configure_wifi.py 配置与 BLE 协议离线测试
data/
  wifi.json         真实凭据（已 gitignore，需自行创建）
  wifi.example.json 配置模板（复制为 wifi.json 并填写）
partitions.csv      分区表：factory 2MB + storage(SPIFFS) 1MB
```

## VS Code ESP-IDF 插件

用 VS Code 打开本项目目录（不要打开其上级目录）。本机 `.vscode/settings.json` 指向已安装的 IDF 配置及 `esp32s3` CMake Preset，属于个人配置，已忽略。

## ESP32-S3 USB-JTAG 断点调试

已完成配置并验证 VS Code 断点调试正常。

### 连接与配置

- 连接开发板的 ESP32-S3 原生 USB / USB&OTG Type-C 接口，不是 CH343P USB 转串口接口。
- 用 VS Code 打开项目根目录，构建目录为 `build/esp32s3`。
- 当前 `sdkconfig` 已启用 `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`。

在 `.vscode/settings.json` 中保留原有配置，增加：

```json
"idf.openOcdConfigs": [
  "board/esp32s3-builtin.cfg"
]
```

创建 `.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "ESP32-S3 断点调试",
      "type": "gdbtarget",
      "request": "attach",
      "program": "${workspaceFolder}/build/esp32s3/esp32s3_app.elf",
      "gdb": "${command:espIdf.getToolchainGdb}",
      "initialBreakpoint": "app_main"
    }
  ]
}
```

### 日常调试

1. 修改代码后先编译、烧录，确保板上固件与本地 ELF 一致。
2. 命令面板执行 `ESP-IDF: OpenOCD Manager`，启动 OpenOCD。
3. 在可执行语句左侧点击设置断点，选择“ESP32-S3 断点调试”，按 F5。
4. 在 `app_main` 停住后，按 F5 继续；F10 单步跳过，F11 进入函数，Shift+F11 跳出函数。
5. 停住时在“变量”“监视”和“调用堆栈”中查看运行状态；Shift+F5 结束调试。

### 常见问题

**1. OpenOCD 报 `libusb_open() failed with LIBUSB_ERROR_NOT_FOUND`**

本机能识别 `USB JTAG/serial debug unit`，但官方驱动安装程序提示“Already installed”后，设备仍绑定原来的 `winusb.inf`，OpenOCD 无法访问。

处理：设备管理器 → `USB JTAG/serial debug unit` → 更新驱动 → 浏览我的电脑 → 让我选取 → 从磁盘安装，选择 ESP-IDF 安装目录下的官方驱动：

```text
idf-driver\idf-driver-esp32-usb-jtag-2021-07-15\usb_jtag_debug_unit.inf
```

不要修改 USB 转串口设备的驱动。驱动目录以本机 ESP-IDF 安装位置为准。

**2. 提示 OpenOCD 未运行**

先通过 `ESP-IDF: OpenOCD Manager` 启动服务；如果启动失败，先排查 OpenOCD 输出，不要反复启动 GDB。

**3. 找不到 ROM ELF 文件**

错误路径为 `esp-rom-elfs/20241011esp32s3_rev0_rom.elf`，原因是 `ESP_ROM_ELF_DIR` 末尾缺少 `/`，生成调试脚本时目录与文件名直接拼接。

以下为路径模板，需将占位符替换为实际工具目录和版本目录，末尾保留 `/`：

```json
"ESP_ROM_ELF_DIR": "<IDF_TOOLS_PATH>/esp-rom-elfs/<版本目录>/"
```

检查 `CMakePresets.json` 中的环境变量；若 `.vscode/settings.json` 的 `idf.customExtraVars` 也配置了该变量，同样保留末尾 `/`，避免后续重新生成时再次出错。

重新配置后，应确认生成的 `build/esp32s3/gdbinit/symbols` 中 ROM ELF 路径正确，路径结构如下：

```text
<IDF_TOOLS_PATH>/esp-rom-elfs/<版本目录>/esp32s3_rev0_rom.elf
```

仅修正调试符号路径不需要重新烧录；修改固件代码则需要重新编译、烧录。

## clangd

项目的 `.clangd` 使用 `build/esp32s3/compile_commands.json` 提供 ESP-IDF 的真实头文件、宏和语言标准，并过滤 clangd 不支持的 GCC 专用参数。不要统一追加 `-std=c++23`，否则 C 文件会被错误地使用 C++ 标准解析。先执行一次上述 CMake 构建以生成编译数据库。

> clangd 红色误报处理：`.vscode/settings.json` 为 clangd 配置了 ESP32-S3 的交叉编译器 `--query-driver`，让 clangd 使用 Xtensa 工具链的 C++ 标准库头文件，而不是 Windows 主机头文件。该文件包含本机路径，已被 `.gitignore` 忽略。修改后请执行 **命令面板 → clangd: Restart language server**。


> 标准头文件解析：项目已配置 `CompileFlags.BuiltinHeaders: QueryDriver`（需要支持该选项的 clangd）。配合 `--query-driver`，直接使用交叉编译器的内置头文件搜索路径；本项目已验证可在不设置个人绝对路径 `--resource-dir` 的情况下解析 `float.h`。这也避免本机 VS Code 启动参数丢失后再次出现同类误报。修改 `.clangd` 后，重启语言服务器并确认编辑器中的诊断；若仍有红线，应进一步检查具体文件及其编译命令。

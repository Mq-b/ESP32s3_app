# 构建、烧录与首次运行

[返回项目首页](../README.md)

目标 ESP32-S3，开发环境为 ESP-IDF v6.1。下列命令从项目根目录执行。

CMake Preset 和 VS Code 设置包含本机路径，不随 Git 分发；使用 Preset 前需按自己的 ESP-IDF、Python 和工具链安装路径配置。未配置 Preset 时，可在已初始化的 ESP-IDF PowerShell 中使用 `idf.py`。

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

或在已初始化环境的 ESP-IDF PowerShell 终端中执行（注意指定构建目录为 preset 的 `build/esp32s3`）：

```powershell
$env:PROCESSOR_ARCHITECTURE = 'AMD64'
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

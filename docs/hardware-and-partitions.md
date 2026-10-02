# 硬件与 Flash 分区

[返回项目首页](../README.md)

## flash 分区布局（8MB）

![flash layout](../images/flash_layout.svg)

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

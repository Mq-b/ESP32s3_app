# 系统资源监控

首页每 2 秒请求 `GET /api/system`，无需安装第三方监控库。不创建后台任务、不等待采样，采集器仅由 HTTP 服务任务串行调用；页面隐藏时暂停资源请求，失败时清除旧数据。

## 指标与口径

| JSON 字段 | 含义 |
|---|---|
| `uptime_ms` | 启动以来的单调时钟毫秒数 |
| `task_count` | 当前 FreeRTOS 任务数量 |
| `memory.internal` | 内部、可按字节访问的动态堆；不是芯片物理 RAM 总量 |
| `memory.psram` | PSRAM 动态堆；总量为 0 表示未启用或没有可分配容量 |
| `*.total_bytes` / `*.free_bytes` | 当前可分配堆总量／空闲字节数 |
| `*.minimum_free_bytes` | 堆历史最低空闲量（多堆区域各自低水位之和，未必同时发生） |
| `*.largest_free_block_bytes` | 最大连续空闲块，可辅助排查碎片化 |
| `storage.available` | storage 分区 SPIFFS 是否可读取 |
| `storage.total_bytes` / `storage.used_bytes` | SPIFFS 报告的容量／使用量，包含文件系统开销，不包含固件和 NVS |
| `flash_bytes` | Flash 芯片检测容量；读取失败为 null，不代表文件系统容量 |
| `cpu.available` | 是否已经得到有效 CPU 采样；首个请求为 false |
| `cpu.cores_percent` | 各核负载百分比，首次或统计关闭为 null 数组 |
| `cpu.usage_percent` | 各核负载算术平均，范围 0–100；并非两核相加 |
| `cpu.sample_window_ms` | CPU 相邻采样时间间隔，通常约 2 秒 |
| `cpu.sample_age_ms` | 最近 CPU 样本距当前请求的时间 |

CPU 估算公式：`100 × (1 − 空闲任务累计运行时间增量 / 采样间隔)`，各核独立计算并限制在 0–100。间隔至少 1 秒才更新，多个浏览器不会把采样窗口缩短到毫秒级；间隔不足则复用 CPU 样本，其他指标重新读取。长时间没有访问后首次结果代表整个访问间隔内的平均负载，而不是最近一秒。

空闲任务运行计数在任务切换时更新，因此存在采样边界误差；中断耗时、临界区等也会影响估算。这是调度忙闲趋势，不是精确的 CPU 指令利用率、实时性保证或任务级性能剖析。监控请求自身及启用运行统计均存在开销。

## 构建配置

`sdkconfig.defaults` 启用：

```text
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y
CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y
```

现有 `sdkconfig` 不会被 defaults 自动覆盖；已有构建需在 menuconfig 开启相同选项后重新构建。本次本地配置已同步。64 位微秒计数避免 32 位计数约 71 分钟回绕。本实现使用当前项目 ESP-IDF 的每核空闲运行计数接口，针对当前非 SMP FreeRTOS 配置；升级至 SMP 内核前需适配采集接口。

## 访问与验证

烧录后浏览器访问设备首页，或请求：

```text
GET http://<设备IP>/api/system
```

成功返回 `application/json`，带 `Cache-Control: no-store`。首次 CPU 为 null，第二次相隔至少一秒请求后开始输出负载。未挂载 SPIFFS、无 PSRAM 或 CPU 统计关闭时，页面显示不可用而不是虚假的 0% 使用率。

建议在实机分别验证：空闲／持续扫描负载、双核负载变化、SPIFFS 文件增减、多个浏览器访问、断网恢复、长时间运行。当前接口仅适用于可信局域网，与原有接口一样未添加认证，不应直接暴露到公网。

前端离线测试（需要 Node.js）：

```powershell
node --test tests/test_system_monitor_ui.cjs
```

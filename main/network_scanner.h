#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief 单台设备的扫描结果
 */
struct DeviceInfo {
    uint32_t ipHost{};   // IP（人类序：192 为最高字节）
    uint8_t mac[6]{};    // MAC 地址
    uint8_t ttl{};       // echo 回复中的 TTL（用于猜测系统）
    uint32_t rttUs{};    // 往返延迟（微秒）
    bool viaPing{};      // true=响应 ICMP；false=仅 ARP 可见
    std::string osGuess; // 按 TTL 猜测的系统
};

/**
 * @brief 内网设备扫描器：ICMP ping /24 网段 + ARP 表收集 MAC。
 *        单例，结果读写线程安全。
 */
class NetworkScanner {
public:
    static NetworkScanner &instance();

    NetworkScanner(const NetworkScanner &) = delete;
    NetworkScanner &operator=(const NetworkScanner &) = delete;

    /// 异步启动一次网段扫描（进行中则忽略）
    void startAsync();

    /// 是否正在扫描
    bool busy() const { return busy_; }

    /// 当前结果（线程安全拷贝）
    std::vector<DeviceInfo> devices() const;

    /// 结果序列化为 JSON（含 scanning / count）
    std::string devicesJson() const;

private:
    NetworkScanner() = default;

    static void scanTaskEntry(void *arg);
    void runScan();

    mutable std::mutex mutex_;
    std::vector<DeviceInfo> devices_;
    std::atomic<bool> busy_{false};
};

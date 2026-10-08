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
 * @brief 内网设备扫描器：主动 ARP 与 ICMP 探测 /24 网段并收集 MAC。
 *        单例，结果读写线程安全。
 */
class NetworkScanner {
public:
    /**
     * @brief 获取线程安全的扫描器单例。
     * @return 进程生命周期内有效的扫描器引用。
     */
    static NetworkScanner &instance();

    NetworkScanner(const NetworkScanner &) = delete;
    NetworkScanner &operator=(const NetworkScanner &) = delete;

    /**
     * @brief 异步启动一次主动 ARP 与 ICMP 网段扫描。
     * @note 已有扫描进行中时忽略请求；结果在整轮完成后统一替换。
     */
    void startAsync();

    /**
     * @brief 查询是否正在扫描。
     * @return 扫描任务运行期间为 true。
     */
    bool busy() const { return busy_; }

    /**
     * @brief 获取最近完成的一轮扫描结果的线程安全拷贝。
     * @return 按 IP 升序排列的设备列表。
     */
    std::vector<DeviceInfo> devices() const;

    /**
     * @brief 序列化当前设备结果与扫描状态。
     * @return 含 scanning、count 与 devices 字段的 JSON 字符串。
     */
    std::string devicesJson() const;

private:
    NetworkScanner() = default;

    static void scanTaskEntry(void *arg);
    void runScan();

    mutable std::mutex mutex_;
    std::vector<DeviceInfo> devices_;
    std::atomic<bool> busy_{false};
};

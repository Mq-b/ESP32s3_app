#include "network_scanner.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

static const char *TAG = "SCAN";

namespace {

constexpr uint32_t PING_BATCH = 4; // 留出 ARP 表空间，避免背景流量挤掉本批响应
constexpr uint32_t PING_ROUND_MS = 300; // 每批收包窗口
constexpr uint16_t PING_ID = 0xE532;    // ICMP id，过滤他人流量
constexpr uint32_t PROBE_ATTEMPTS = 2; // 对瞬时丢包和休眠设备补探一次
constexpr size_t MAX_DEVICES = 254;

/**
 * @brief ICMP echo 报文（收发同构）
 */
struct __attribute__((packed)) IcmpEcho {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;  // 复用为目标主机地址末字节（1..254）
    uint32_t tsUs; // 发送时刻（微秒）
};

/**
 * @brief 互联网校验和（RFC 1071）
 */
uint16_t checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += (data[0] << 8) | data[1];
        data += 2;
        len -= 2;
    }
    if (len == 1)
        sum += data[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~static_cast<uint16_t>(sum);
}

/**
 * @brief 按 TTL 猜操作系统（初始值：Windows 128 / 类 Unix 64 / 网络设备 255）
 */
std::string guessOs(uint8_t ttl) {
    if (ttl >= 250)
        return "Network device";
    if (ttl >= 120)
        return "Windows";
    if (ttl >= 60)
        return "Linux/macOS/Mobile";
    return "Unknown";
}

/**
 * @brief 在列表中查找或创建设备条目（IP 为主键）
 * @note  返回指针在后续 push_back 后失效，须立即使用
 */
DeviceInfo *getOrCreate(std::vector<DeviceInfo> &list, uint32_t ipHost) {
    for (auto &d : list) {
        if (d.ipHost == ipHost)
            return &d;
    }
    if (list.size() >= MAX_DEVICES)
        return nullptr;
    DeviceInfo d{};
    d.ipHost = ipHost;
    list.push_back(d);
    return &list.back();
}

std::string ipToString(uint32_t ipHost) {
    return std::format("{}.{}.{}.{}", (ipHost >> 24) & 0xFF,
                       (ipHost >> 16) & 0xFF, (ipHost >> 8) & 0xFF,
                       ipHost & 0xFF);
}

/**
 * @brief 确保网络扫描任务的 raw socket 在异常和提前返回时关闭。
 */
struct SocketGuard {
    int fd = -1;
    explicit SocketGuard(int value) : fd(value) {}
    ~SocketGuard() {
        if (fd >= 0)
            lwip_close(fd);
    }
    SocketGuard(const SocketGuard &) = delete;
    SocketGuard &operator=(const SocketGuard &) = delete;
};

std::string macToString(const uint8_t mac[6]) {
    return std::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                       (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2],
                       (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
}

/**
 * @brief 发送一个 ICMP echo request
 */
void pingSend(int sock, uint32_t ipHost) {
    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(ipHost);

    IcmpEcho pkt{};
    pkt.type = 8; // echo request
    pkt.id = htons(PING_ID);
    pkt.seq = htons(static_cast<uint16_t>(ipHost & 0xFF));
    pkt.tsUs = static_cast<uint32_t>(esp_timer_get_time());
    pkt.checksum = htons(checksum(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt)));

    lwip_sendto(sock, &pkt, sizeof(pkt), 0,
                reinterpret_cast<struct sockaddr *>(&dst), sizeof(dst));
}

/**
 * @brief 收取当前到达的 echo reply（50ms 内无数据即返回）
 * @note  lwIP raw socket 收包带 IP 头（raw.c 原样投递整个 pbuf），TTL 在偏移 8
 */
void pingRecv(int sock, std::vector<DeviceInfo> &found) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv{.tv_sec = 0, .tv_usec = 50 * 1000};
    if (lwip_select(sock + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return;

    uint8_t buf[128];
    struct sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    int len =
        lwip_recvfrom(sock, buf, sizeof(buf), 0,
                      reinterpret_cast<struct sockaddr *>(&from), &fromLen);
    if (len < 20 + static_cast<int>(sizeof(IcmpEcho)))
        return;

    uint8_t versionIhl = buf[0];
    if ((versionIhl >> 4) != 4)
        return;
    size_t ihl = (versionIhl & 0x0F) * 4;
    if (static_cast<int>(ihl + sizeof(IcmpEcho)) > len)
        return;

    uint32_t srcHost = ntohl(from.sin_addr.s_addr);
    uint8_t ttl = buf[8];
    auto *icmp = reinterpret_cast<IcmpEcho *>(buf + ihl);
    if (icmp->type != 0 || ntohs(icmp->id) != PING_ID)
        return;

    if (DeviceInfo *d = getOrCreate(found, srcHost);
        d != nullptr && !d->viaPing) {
        d->viaPing = true;
        d->ttl = ttl;
        d->rttUs = static_cast<uint32_t>(esp_timer_get_time()) - icmp->tsUs;
    }
}

/**
 * @brief 跨线程传递主动 ARP 探测范围与发送失败数量。
 */
struct ArpProbe {
    struct netif *interface;
    uint32_t netBase;
    uint32_t firstHost;
    uint32_t endHost;
    uint32_t failures{};
};

/**
 * @brief 在 TCP/IP 线程主动探测本批地址，不依赖已有 ARP 缓存状态。
 * @param arg 同步回调期间有效的 ArpProbe 指针。
 */
void probeArpOnTcpip(void *arg) {
    auto &probe = *static_cast<ArpProbe *>(arg);
    for (uint32_t host = probe.firstHost; host < probe.endHost; ++host) {
        ip4_addr_t target{};
        target.addr = htonl(probe.netBase + host);
        // 空报文查询强制发送 ARP 请求，同时为首次出现的地址建立待解析项。
        if (etharp_query(probe.interface, &target, nullptr) != ERR_OK)
            ++probe.failures;
    }
}

/** @brief ARP 表快照中的纯数据条目，避免在 TCP/IP 线程分配动态内存。 */
struct ArpEntry {
    uint32_t ipHost{};
    uint8_t mac[6]{};
};

/** @brief 同步快照上下文，仅收集扫描接口和当前 /24 网段的条目。 */
struct ArpSnapshot {
    struct netif *interface;
    uint32_t netBase;
    std::array<ArpEntry, ARP_TABLE_SIZE> entries{};
    size_t count{};
};

/**
 * @brief 在 TCP/IP 线程拷贝 ARP 表，避免与收包和老化过程并发访问。
 * @param arg 同步回调期间有效的 ArpSnapshot 指针。
 */
void snapshotArpOnTcpip(void *arg) {
    auto &snapshot = *static_cast<ArpSnapshot *>(arg);
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip = nullptr;
        struct netif *interface = nullptr;
        struct eth_addr *mac = nullptr;
        if (!etharp_get_entry(i, &ip, &interface, &mac) ||
            ip == nullptr || mac == nullptr || interface != snapshot.interface)
            continue;
        uint32_t ipHost = ntohl(ip->addr);
        if ((ipHost & 0xFFFFFF00) != snapshot.netBase ||
            (ipHost & 0xFF) == 0 || (ipHost & 0xFF) == 255)
            continue;
        auto &entry = snapshot.entries[snapshot.count++];
        entry.ipHost = ipHost;
        memcpy(entry.mac, mac->addr, sizeof(entry.mac));
    }
}

/**
 * @brief 同步获取 ARP 快照，并在扫描任务中合并设备结果。
 * @param found 当前轮的设备列表，不跨轮保留历史设备。
 * @param interface 本轮扫描使用的 lwIP 网络接口。
 * @param netBase 当前 /24 网段的主机字节序网络地址。
 */
void snapshotArp(std::vector<DeviceInfo> &found, struct netif *interface,
                 uint32_t netBase) {
    ArpSnapshot snapshot{interface, netBase};
    if (tcpip_callback_wait(snapshotArpOnTcpip, &snapshot) != ERR_OK) {
        ESP_LOGW(TAG, "获取 ARP 快照失败");
        return;
    }
    for (size_t i = 0; i < snapshot.count; ++i) {
        const auto &entry = snapshot.entries[i];
        if (DeviceInfo *device = getOrCreate(found, entry.ipHost))
            memcpy(device->mac, entry.mac, sizeof(entry.mac));
    }
}

} // namespace

NetworkScanner &NetworkScanner::instance() {
    static NetworkScanner scanner;
    return scanner;
}

void NetworkScanner::startAsync() {
    if (busy_.exchange(true)) {
        ESP_LOGW(TAG, "扫描正在进行中");
        return;
    }

    if (xTaskCreate(scanTaskEntry, "scan", 8192, this, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "创建扫描任务失败");
        busy_ = false;
    }
}

void NetworkScanner::scanTaskEntry(void *arg) {
    auto *scanner = static_cast<NetworkScanner *>(arg);
    // 先正常返回，确保 socket 和局部容器析构，再释放忙状态并删除任务。
    scanner->runScan();
    scanner->busy_ = false;
    vTaskDelete(nullptr);
}

void NetworkScanner::runScan() {
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ipInfo{};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &ipInfo) != ESP_OK) {
        ESP_LOGE(TAG, "无 IP，扫描取消");
        return;
    }
    auto *interface = static_cast<struct netif *>(esp_netif_get_netif_impl(netif));
    if (interface == nullptr || ipInfo.ip.addr == 0) {
        ESP_LOGE(TAG, "网络接口未就绪，扫描取消");
        return;
    }
    uint32_t selfHost = ntohl(ipInfo.ip.addr);
    uint32_t netBase = selfHost & 0xFFFFFF00;

    SocketGuard socketGuard{lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)};
    int sock = socketGuard.fd;
    if (sock < 0) {
        ESP_LOGE(TAG, "raw socket 创建失败: %s", strerror(errno));
        return;
    }

    ESP_LOGI(TAG, "%s",
             std::format("开始扫描 {}.{}.{}.0/24", (netBase >> 24) & 0xFF,
                         (netBase >> 16) & 0xFF, (netBase >> 8) & 0xFF)
                 .c_str());

    std::vector<DeviceInfo> found;
    found.reserve(32);

    // 每批主动 ARP + ICMP，周期快照及时保存响应，防止小容量 ARP 表覆盖条目。
    for (uint32_t base = 1; base <= 254; base += PING_BATCH) {
        uint32_t endHost = std::min(base + PING_BATCH, uint32_t{255});
        for (uint32_t attempt = 0; attempt < PROBE_ATTEMPTS; ++attempt) {
            ArpProbe probe{interface, netBase, base, endHost};
            if (tcpip_callback_wait(probeArpOnTcpip, &probe) != ERR_OK ||
                probe.failures != 0)
                ESP_LOGW(TAG, "本批 ARP 探测发送失败，起始地址末字节: %lu",
                         static_cast<unsigned long>(base));
            for (uint32_t host = base; host < endHost; ++host) {
                if (netBase + host != selfHost)
                    pingSend(sock, netBase + host);
            }
            // 使用 64 位时间，避免运行约 71 分钟后微秒计数截断导致窗口失效。
            int64_t deadline = esp_timer_get_time() + PING_ROUND_MS * 1000;
            while (esp_timer_get_time() < deadline) {
                pingRecv(sock, found);
                snapshotArp(found, interface, netBase);
            }
        }
    }

    // 收尾期间也持续快照，接住晚到的 ARP 和 ICMP 响应。
    int64_t deadline = esp_timer_get_time() + 1000 * 1000;
    while (esp_timer_get_time() < deadline) {
        pingRecv(sock, found);
        snapshotArp(found, interface, netBase);
    }

    // 本机自己也列出来
    if (DeviceInfo *self = getOrCreate(found, selfHost)) {
        esp_read_mac(self->mac, ESP_MAC_WIFI_STA);
        self->viaPing = true;
        self->ttl = 64;
        self->rttUs = 0;
    }

    for (auto &d : found) {
        d.osGuess = (d.ipHost == selfHost) ? "ESP32-S3 (self)"
                    : d.viaPing            ? guessOs(d.ttl)
                                           : "No ICMP (ARP only)";
    }

    // IP 升序
    std::sort(found.begin(), found.end(),
              [](const DeviceInfo &a, const DeviceInfo &b) {
                  return a.ipHost < b.ipHost;
              });

    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        devices_ = std::move(found);
        count = devices_.size();
    }

    ESP_LOGI(TAG, "%s", std::format("扫描完成，发现 {} 台设备", count).c_str());
}

std::vector<DeviceInfo> NetworkScanner::devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

std::string NetworkScanner::devicesJson() const {
    std::vector<DeviceInfo> copy = devices();

    using Json = nlohmann::ordered_json;
    Json entries = Json::array();
    for (const auto &device : copy) {
        entries.push_back({{"ip", ipToString(device.ipHost)},
                           {"mac", macToString(device.mac)},
                           {"ttl", device.ttl},
                           {"rtt_ms", device.rttUs / 1000.0},
                           {"ping", device.viaPing},
                           {"os", device.osGuess}});
    }
    const Json root = {{"scanning", busy_.load()},
                       {"count", copy.size()},
                       {"devices", std::move(entries)}};
    return root.dump(-1, ' ', false, Json::error_handler_t::replace);
}

#include "network_scanner.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/sockets.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

static const char *TAG = "SCAN";

namespace {

constexpr uint32_t PING_BATCH = 8;       // 每批 ping 数（须 < ARP 表 10 格，防挤掉）
constexpr uint32_t PING_ROUND_MS = 300;  // 每批收包窗口
constexpr uint16_t PING_ID = 0xE532;     // ICMP id，过滤他人流量
constexpr size_t MAX_DEVICES = 64;

/**
 * @brief ICMP echo 报文（收发同构）
 */
struct __attribute__((packed)) IcmpEcho {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;   // 复用为目标主机地址末字节（1..254）
    uint32_t tsUs;  // 发送时刻（微秒）
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
    if (len == 1) sum += data[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~static_cast<uint16_t>(sum);
}

/**
 * @brief 按 TTL 猜操作系统（初始值：Windows 128 / 类 Unix 64 / 网络设备 255）
 */
std::string guessOs(uint8_t ttl) {
    if (ttl >= 250) return "Network device";
    if (ttl >= 120) return "Windows";
    if (ttl >= 60) return "Linux/macOS/Mobile";
    return "Unknown";
}

/**
 * @brief 在列表中查找或创建设备条目（IP 为主键）
 * @note  返回指针在后续 push_back 后失效，须立即使用
 */
DeviceInfo *getOrCreate(std::vector<DeviceInfo> &list, uint32_t ipHost) {
    for (auto &d : list) {
        if (d.ipHost == ipHost) return &d;
    }
    if (list.size() >= MAX_DEVICES) return nullptr;
    DeviceInfo d {};
    d.ipHost = ipHost;
    list.push_back(d);
    return &list.back();
}

std::string ipToString(uint32_t ipHost) {
    return std::format("{}.{}.{}.{}", (ipHost >> 24) & 0xFF, (ipHost >> 16) & 0xFF,
                       (ipHost >> 8) & 0xFF, ipHost & 0xFF);
}

std::string macToString(const uint8_t mac[6]) {
    return std::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", (unsigned)mac[0],
                       (unsigned)mac[1], (unsigned)mac[2], (unsigned)mac[3],
                       (unsigned)mac[4], (unsigned)mac[5]);
}

/**
 * @brief 发送一个 ICMP echo request
 */
void pingSend(int sock, uint32_t ipHost) {
    struct sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(ipHost);

    IcmpEcho pkt {};
    pkt.type = 8;  // echo request
    pkt.id = PING_ID;
    pkt.seq = static_cast<uint16_t>(ipHost & 0xFF);
    pkt.tsUs = static_cast<uint32_t>(esp_timer_get_time());
    pkt.checksum = checksum(reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));

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
    struct timeval tv { .tv_sec = 0, .tv_usec = 50 * 1000 };
    if (lwip_select(sock + 1, &fds, nullptr, nullptr, &tv) <= 0) return;

    uint8_t buf[128];
    struct sockaddr_in from {};
    socklen_t fromLen = sizeof(from);
    int len = lwip_recvfrom(sock, buf, sizeof(buf), 0,
                            reinterpret_cast<struct sockaddr *>(&from), &fromLen);
    if (len < 20 + static_cast<int>(sizeof(IcmpEcho))) return;

    uint8_t versionIhl = buf[0];
    if ((versionIhl >> 4) != 4) return;
    size_t ihl = (versionIhl & 0x0F) * 4;
    if (static_cast<int>(ihl + sizeof(IcmpEcho)) > len) return;

    uint32_t srcHost = ntohl(from.sin_addr.s_addr);
    uint8_t ttl = buf[8];
    auto *icmp = reinterpret_cast<IcmpEcho *>(buf + ihl);
    if (icmp->type != 0 || icmp->id != PING_ID) return;

    if (DeviceInfo *d = getOrCreate(found, srcHost); d != nullptr && !d->viaPing) {
        d->viaPing = true;
        d->ttl = ttl;
        d->rttUs = static_cast<uint32_t>(esp_timer_get_time()) - icmp->tsUs;
    }
}

/**
 * @brief 快照 lwIP ARP 表，把 (IP, MAC) 合并进结果。
 *        每批 ping 后调用，绕开 ARP 表条目少被挤掉的问题。
 */
void snapshotArp(std::vector<DeviceInfo> &found) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip = nullptr;
        struct netif *netif = nullptr;
        struct eth_addr *mac = nullptr;
        if (!etharp_get_entry(i, &ip, &netif, &mac)) continue;
        if (ip == nullptr || mac == nullptr || netif == nullptr) continue;

        uint32_t ipHost = ntohl(ip->addr);
        if (DeviceInfo *d = getOrCreate(found, ipHost)) {
            memcpy(d->mac, mac->addr, 6);
        }
    }
}

}  // namespace

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
    static_cast<NetworkScanner *>(arg)->runScan();
}

void NetworkScanner::runScan() {
    esp_netif_t *netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ipInfo {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &ipInfo) != ESP_OK) {
        ESP_LOGE(TAG, "无 IP，扫描取消");
        busy_ = false;
        vTaskDelete(nullptr);
        return;
    }
    uint32_t selfHost = ntohl(ipInfo.ip.addr);
    uint32_t netBase = selfHost & 0xFFFFFF00;

    int sock = lwip_socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        ESP_LOGE(TAG, "raw socket 创建失败: %s", strerror(errno));
        busy_ = false;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "%s", std::format("开始扫描 {}.{}.{}.0/24", (netBase >> 24) & 0xFF,
                                    (netBase >> 16) & 0xFF, (netBase >> 8) & 0xFF)
                          .c_str());

    std::vector<DeviceInfo> found;
    found.reserve(32);

    // 分批：8 个一批，批间快照 ARP（表只有 10 格，批大了会挤掉旧条目）
    for (uint32_t base = 1; base <= 254; base += PING_BATCH) {
        for (uint32_t h = base; h < base + PING_BATCH && h <= 254; h++) {
            pingSend(sock, netBase + h);
        }
        uint32_t deadline =
            static_cast<uint32_t>(esp_timer_get_time()) + PING_ROUND_MS * 1000;
        while (static_cast<uint32_t>(esp_timer_get_time()) < deadline) {
            pingRecv(sock, found);
        }
        snapshotArp(found);
    }

    // 收尾：晚到的回包 + 最终 ARP
    uint32_t deadline = static_cast<uint32_t>(esp_timer_get_time()) + 1000 * 1000;
    while (static_cast<uint32_t>(esp_timer_get_time()) < deadline) {
        pingRecv(sock, found);
    }
    lwip_close(sock);
    snapshotArp(found);

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
              [](const DeviceInfo &a, const DeviceInfo &b) { return a.ipHost < b.ipHost; });

    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        devices_ = std::move(found);
        count = devices_.size();
    }

    ESP_LOGI(TAG, "%s", std::format("扫描完成，发现 {} 台设备", count).c_str());
    busy_ = false;
    vTaskDelete(nullptr);
}

std::vector<DeviceInfo> NetworkScanner::devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

std::string NetworkScanner::devicesJson() const {
    std::vector<DeviceInfo> copy = devices();

    std::string json = std::format("{{\"scanning\":{},\"count\":{},\"devices\":[",
                                   busy_.load(), copy.size());
    for (size_t i = 0; i < copy.size(); i++) {
        const auto &d = copy[i];
        json += std::format(
            "{{\"ip\":\"{}\",\"mac\":\"{}\",\"ttl\":{},\"rtt_ms\":{}.{:03},\"ping\":{},\"os\":\"{}\"}}",
            ipToString(d.ipHost), macToString(d.mac), (unsigned)d.ttl, d.rttUs / 1000,
            d.rttUs % 1000, d.viaPing, d.osGuess);
        if (i + 1 < copy.size()) json += ",";
    }
    json += "]}";
    return json;
}

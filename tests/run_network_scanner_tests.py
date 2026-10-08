"""编译真实网络扫描实现，通过 lwIP 测试桩验证重复扫描与 ICMP 字节序。"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

STUBS = {
    "esp_log.h": """#pragma once
template <typename... Args> inline void scanner_test_log(Args &&...) {}
#define ESP_LOGW(...) scanner_test_log(__VA_ARGS__)
#define ESP_LOGE(...) scanner_test_log(__VA_ARGS__)
#define ESP_LOGI(...) scanner_test_log(__VA_ARGS__)
""",
    "esp_mac.h": """#pragma once
#include <cstring>
#define ESP_MAC_WIFI_STA 0
inline int esp_read_mac(uint8_t *mac, int) {
    memset(mac, 0xAA, 6);
    return 0;
}
""",
    "freertos/FreeRTOS.h": """#pragma once
#define pdPASS 1
""",
    "freertos/task.h": """#pragma once
inline int xTaskCreate(void (*)(void *), const char *, int, void *, int,
                       void *) {
    return 1;
}
inline void vTaskDelete(void *) {}
""",
    "esp_timer.h": """#pragma once
#include "scanner_test_state.h"
inline int64_t esp_timer_get_time() { return test_scan::now; }
""",
    "esp_netif.h": """#pragma once
#include "scanner_test_state.h"
#define ESP_OK 0
struct esp_netif_t {};
struct esp_netif_ip_info_t {
    ip4_addr_t ip;
};
inline esp_netif_t *esp_netif_get_default_netif() {
    static esp_netif_t value;
    return &value;
}
inline int esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *info) {
    info->ip.addr = htonl(test_scan::network + 10);
    return 0;
}
""",
    "esp_netif_net_stack.h": """#pragma once
#include "esp_netif.h"
inline void *esp_netif_get_netif_impl(esp_netif_t *) {
    return &test_scan::interface;
}
""",
    "arpa/inet.h": """#pragma once
#include <cstdint>
inline uint16_t htons(uint16_t x) { return (x >> 8) | (x << 8); }
inline uint16_t ntohs(uint16_t x) { return htons(x); }
inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
inline uint32_t ntohl(uint32_t x) { return htonl(x); }
""",
    "lwip/tcpip.h": """#pragma once
#include "scanner_test_state.h"
inline int tcpip_callback_wait(void (*callback)(void *), void *arg) {
    assert(!test_scan::onTcpip);
    test_scan::onTcpip = true;
    callback(arg);
    test_scan::onTcpip = false;
    return ERR_OK;
}
""",
    "lwip/etharp.h": """#pragma once
#include "scanner_test_state.h"
inline int etharp_query(netif *interface, const ip4_addr_t *ip, void *packet) {
    assert(test_scan::onTcpip && interface == &test_scan::interface &&
           packet == nullptr);
    unsigned host = ntohl(ip->addr) & 255;
    ++test_scan::queries[host];
    if (test_scan::online[host] &&
        (host != 106 || test_scan::queries[host] % 2 == 0))
        test_scan::cache(ntohl(ip->addr));
    return ERR_OK;
}
inline int etharp_get_entry(int index, ip4_addr_t **ip, netif **interface,
                            eth_addr **mac) {
    assert(test_scan::onTcpip);
    auto &entry = test_scan::table[index];
    if (!entry.valid)
        return 0;
    *ip = &entry.ip;
    *interface = entry.interface;
    *mac = &entry.mac;
    return 1;
}
""",
    "lwip/sockets.h": """#pragma once
#include "scanner_test_state.h"
#include <cstring>
#define AF_INET 2
#define SOCK_RAW 3
#define IPPROTO_ICMP 1
struct sockaddr {};
struct sockaddr_in {
    int sin_family;
    struct {
        uint32_t s_addr;
    } sin_addr;
};
using socklen_t = unsigned;
#ifdef _WIN32
struct fd_set {};
#define FD_ZERO(set) ((void)(set))
#define FD_SET(fd, set) ((void)(fd), (void)(set))
#else
#include <sys/select.h>
#endif
inline int lwip_socket(int, int, int) { return 3; }
inline int lwip_close(int) { return 0; }
inline int lwip_select(int, fd_set *, void *, void *, timeval *) {
    test_scan::now += 50000;
    return !test_scan::packets.empty();
}
inline int lwip_sendto(int, const void *data, size_t size, int,
                       const sockaddr *address, size_t) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i += 2)
        sum += (bytes[i] << 8) | bytes[i + 1];
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    assert(sum == 65535);
    assert(bytes[4] == 0xE5 && bytes[5] == 0x32);
    ++test_scan::sent;
    auto host =
        ntohl(reinterpret_cast<const sockaddr_in *>(address)->sin_addr.s_addr) &
        255;
    if (host == 2 && test_scan::online[host]) {
        test_scan::Packet packet{};
        packet.host = test_scan::network + host;
        packet.bytes[0] = 0x45;
        packet.bytes[8] = 64;
        memcpy(packet.bytes.data() + 20, data, size);
        packet.bytes[20] = 0;
        test_scan::packets.push_back(packet);
    }
    return static_cast<int>(size);
}
inline int lwip_recvfrom(int, void *data, size_t, int, sockaddr *from,
                         socklen_t *) {
    auto packet = test_scan::packets.front();
    test_scan::packets.erase(test_scan::packets.begin());
    memcpy(data, packet.bytes.data(), packet.bytes.size());
    reinterpret_cast<sockaddr_in *>(from)->sin_addr.s_addr = htonl(packet.host);
    return static_cast<int>(packet.bytes.size());
}
""",
}

STATE = r"""#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
#include "arpa/inet.h"
#define ARP_TABLE_SIZE 10
#define ERR_OK 0
struct ip4_addr_t { uint32_t addr{}; };
struct netif {};
struct eth_addr { uint8_t addr[6]{}; };
namespace test_scan {
inline constexpr uint32_t network = 0xC0A80000;
inline int64_t now = (int64_t{1} << 32) - 100000;
inline bool onTcpip = false;
inline netif interface, foreignInterface;
inline std::array<bool, 256> online{};
inline std::array<unsigned, 256> queries{};
inline unsigned sent = 0;
struct Entry { ip4_addr_t ip; eth_addr mac; netif *interface; bool valid; };
inline std::array<Entry, ARP_TABLE_SIZE> table{};
inline unsigned nextEntry = 0;
struct Packet { uint32_t host; std::array<uint8_t, 32> bytes; };
inline std::vector<Packet> packets;
inline void cache(uint32_t host) {
    Entry *entry = nullptr;
    for (auto &candidate : table) if (candidate.valid && ntohl(candidate.ip.addr) == host) entry = &candidate;
    if (!entry) entry = &table[nextEntry++ % table.size()];
    *entry = Entry{{htonl(host)}, {{0x02, 0x03, 0x04, 0x05, 0x06, static_cast<uint8_t>(host)}}, &interface, true};
}
}
"""


def main():
    """在临时目录构建测试，校验真实实现而不是复制扫描算法。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    component = root / "managed_components" / "mittelab__nlohmann-json"
    with tempfile.TemporaryDirectory(prefix="network-scanner-tests-") as directory:
        temporary = Path(directory)
        for name, content in {**STUBS, "scanner_test_state.h": STATE}.items():
            path = temporary / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        executable = temporary / ("test_network_scanner" + (".exe" if os.name == "nt" else ""))
        subprocess.run([args.cxx, "-std=c++23", "-fno-exceptions", "-Wall", "-Wextra", "-Werror",
                        "-I", str(temporary), "-I", str(root / "main"), "-I", str(component),
                        str(root / "tests" / "test_network_scanner.cpp"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()

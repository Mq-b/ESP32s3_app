#include "scanner_test_state.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>

// 仅测试入口开放私有成员，直接运行生产扫描逻辑，不启动 FreeRTOS 任务。
#define private public
#include "network_scanner.h"
#undef private
#include "network_scanner.cpp"

/**
 * @brief 验证重复主动扫描、ARP 小表覆盖、补探、ICMP 字节序与时间溢出。
 * @return 全部断言通过时返回 0。
 */
int main() {
    using namespace test_scan;
    online[1] = online[2] = online[106] = online[250] = true;
    for (unsigned host = 20; host <= 100; ++host)
        online[host] = true;
    NetworkScanner scanner;
    for (unsigned round = 1; round <= 3; ++round) {
        scanner.runScan();
        const auto devices = scanner.devices();
        assert(devices.size() == 86); // 超过原有 64 台上限。
        for (unsigned host = 1; host <= 254; ++host) {
            assert(queries[host] == round * PROBE_ATTEMPTS);
            auto device = std::find_if(
                devices.begin(), devices.end(),
                [host](const auto &d) { return d.ipHost == network + host; });
            assert((device != devices.end()) == (online[host] || host == 10));
            if (device != devices.end()) {
                assert(device->mac[0] == (host == 10 ? 0xAA : 0x02));
                assert(device->viaPing == (host == 2 || host == 10));
            }
        }
        assert(std::is_sorted(
            devices.begin(), devices.end(),
            [](const auto &a, const auto &b) { return a.ipHost < b.ipHost; }));
        auto json = nlohmann::json::parse(scanner.devicesJson());
        assert(json["count"] == 86);
        assert(!json["scanning"].get<bool>());
    }
    assert(sent == 3 * 253 * PROBE_ATTEMPTS);

    // 跨接口与跨网段的 ARP 缓存不得混入结果。
    table = {};
    nextEntry = 0;
    cache(network + 1);
    cache(0xC0A80102);
    cache(network + 3);
    table[2].interface = &foreignInterface;
    std::vector<DeviceInfo> filtered;
    snapshotArp(filtered, &interface, network);
    assert(filtered.size() == 1 && filtered[0].ipHost == network + 1);

    // 新一轮不合并上一轮结果；清空模拟缓存后，只保留当前可响应设备和本机。
    table = {};
    online = {};
    online[2] = true;
    scanner.runScan();
    assert(scanner.devices().size() == 2);
    std::cout << "网络扫描回归测试通过：连续扫描、小容量 ARP 表、补探、ICMP "
                 "字节序、时间溢出和结果替换\n";
}

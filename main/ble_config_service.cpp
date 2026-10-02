#include "ble_config_service.h"

#include "app_config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" void ble_store_config_init(void);

namespace {

constexpr const char *TAG = "BLE_CONFIG";
constexpr size_t MAX_CONFIG_SIZE = 1024;
// UUID 为 7d9a0001/0002/0003-6f41-4b5b-9c82-56e0438ab100，按 BLE 小端顺序定义。
const ble_uuid128_t serviceUuid = BLE_UUID128_INIT(
    0x00, 0xb1, 0x8a, 0x43, 0xe0, 0x56, 0x82, 0x9c,
    0x5b, 0x4b, 0x41, 0x6f, 0x01, 0x00, 0x9a, 0x7d);
const ble_uuid128_t dataUuid = BLE_UUID128_INIT(
    0x00, 0xb1, 0x8a, 0x43, 0xe0, 0x56, 0x82, 0x9c,
    0x5b, 0x4b, 0x41, 0x6f, 0x02, 0x00, 0x9a, 0x7d);
const ble_uuid128_t controlUuid = BLE_UUID128_INIT(
    0x00, 0xb1, 0x8a, 0x43, 0xe0, 0x56, 0x82, 0x9c,
    0x5b, 0x4b, 0x41, 0x6f, 0x03, 0x00, 0x9a, 0x7d);
uint8_t addressType;
constexpr uint32_t passkey = 123456;
std::string configJson;
size_t readOffset = 0;
constexpr size_t READ_CHUNK_SIZE = 128;
char deviceName[24] = {};
std::string incoming;
std::string status = "idle";
bool saved = false;
bool rebooting = false;

/** @brief 清除传输缓冲区中的凭据，断开连接不会丢弃已提交配置。 */
void clearIncoming() {
    std::fill(incoming.begin(), incoming.end(), '\0');
    incoming.clear();
}

/** @brief 独立任务延迟重启，给 GATT 写响应留出发送时间。 */
void rebootTask(void *) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/** @brief 处理认证加密连接上的完整配置读写及控制命令，凭据禁止写入日志。 */
int accessCharacteristic(uint16_t, uint16_t, ble_gatt_access_ctxt *context, void *arg) {
    const bool control = arg != nullptr;
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (control) {
            return os_mbuf_append(context->om, status.data(), status.size()) == 0 ?
                0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        const size_t size = std::min(READ_CHUNK_SIZE, configJson.size() - readOffset);
        return os_mbuf_append(context->om, configJson.data() + readOffset, size) == 0 ?
            0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (rebooting) return BLE_ATT_ERR_UNLIKELY;
    std::array<char, 513> buffer{};
    uint16_t length = 0;
    if (OS_MBUF_PKTLEN(context->om) == 0 || OS_MBUF_PKTLEN(context->om) > 512) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(context->om, buffer.data(), 512, &length) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!control) {
        if (incoming.size() + length > MAX_CONFIG_SIZE) {
            clearIncoming();
            status = "error:too_large";
            saved = false;
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        incoming.append(buffer.data(), length);
        std::fill(buffer.begin(), buffer.end(), '\0');
        saved = false;
        status = "receiving";
        return 0;
    }
    const std::string command(buffer.data(), length);
    if (command.compare(0, 5, "read:") == 0) {
        const std::string offset = command.substr(5);
        if (offset.empty() || offset.size() > 4 ||
            offset.find_first_not_of("0123456789") != std::string::npos) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        const size_t requested = std::strtoul(offset.c_str(), nullptr, 10);
        if (requested > configJson.size()) return BLE_ATT_ERR_INVALID_OFFSET;
        readOffset = requested;
    } else if (command == "reset") {
        clearIncoming();
        saved = false;
        status = "idle";
    } else if (command == "save") {
        AppConfig config;
        std::string error;
        saved = false;
        if (!AppConfig::parse(incoming, config, error)) {
            status = "error:invalid_config";
            ESP_LOGW(TAG, "蓝牙配置校验失败: %s", error.c_str());
        } else {
            const esp_err_t err = config.save();
            saved = err == ESP_OK;
            if (saved) {
                configJson = config.toJson();
                readOffset = 0;
            }
            status = saved ? "saved" : "error:storage";
            if (!saved) ESP_LOGE(TAG, "配置保存失败: %s", esp_err_to_name(err));
        }
        clearIncoming();
    } else if (command == "reboot" && saved) {
        rebooting = true;
        if (xTaskCreate(rebootTask, "config_reboot", 2048, nullptr, 5, nullptr) != pdPASS) {
            rebooting = false;
            status = "error:reboot_task";
        } else {
            status = "rebooting";
        }
    } else {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

const std::array<ble_gatt_chr_def, 3> characteristics = [] {
    std::array<ble_gatt_chr_def, 3> result{};
    result[0].uuid = &dataUuid.u;
    result[0].access_cb = accessCharacteristic;
    result[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN |
                      BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
    result[0].min_key_size = 16;
    result[1].uuid = &controlUuid.u;
    result[1].access_cb = accessCharacteristic;
    result[1].arg = &addressType;
    result[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN |
                      BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
    result[1].min_key_size = 16;
    return result;
}();
const std::array<ble_gatt_svc_def, 2> services = [] {
    std::array<ble_gatt_svc_def, 2> result{};
    result[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    result[0].uuid = &serviceUuid.u;
    result[0].characteristics = characteristics.data();
    return result;
}();

void advertise();

/** @brief 处理连接、断开和配对输入，连接失败或断开后恢复广播。 */
int gapEvent(ble_gap_event *event, void *) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
        } else {
            const int rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "请求蓝牙认证失败: %d", rc);
            }
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        ble_gap_conn_desc connection{};
        if (ble_gap_conn_find(event->enc_change.conn_handle, &connection) == 0) {
            ESP_LOGI(TAG, "蓝牙安全状态: 结果=%d 加密=%d 认证=%d 绑定=%d",
                     event->enc_change.status, connection.sec_state.encrypted,
                     connection.sec_state.authenticated, connection.sec_state.bonded);
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // 仅清理当前对端旧绑定，允许 Windows 删除配对后用固定码重新认证。
        ble_gap_conn_desc connection{};
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &connection) != 0 ||
            ble_store_util_delete_peer(&connection.peer_id_addr) != 0) {
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        ESP_LOGI(TAG, "对端请求重新配对，已清理该对端旧绑定");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        clearIncoming();
        readOffset = 0;
        saved = false;
        status = "idle";
        if (!rebooting) advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!rebooting) advertise();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            ble_sm_io io{};
            io.action = BLE_SM_IOACT_DISP;
            io.passkey = passkey;
            const int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if (rc != 0) ESP_LOGW(TAG, "配对码提交失败: %d", rc);
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

/** @brief 广播服务 UUID，扫描响应携带设备名，不广播任何网络凭据。 */
void advertise() {
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &serviceUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置蓝牙广播失败: %d", rc);
        return;
    }
    ble_hs_adv_fields response{};
    response.name = reinterpret_cast<const uint8_t *>(deviceName);
    response.name_len = std::strlen(deviceName);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc == 0) {
        ble_gap_adv_params params{};
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        params.itvl_min = 320;
        params.itvl_max = 480;
        rc = ble_gap_adv_start(addressType, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
    }
    if (rc != 0) ESP_LOGE(TAG, "启动蓝牙广播失败: %d", rc);
}

/** @brief 主机与控制器同步后选择地址并开始广播。 */
void onSync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &addressType);
    if (rc == 0) advertise();
    else ESP_LOGE(TAG, "蓝牙地址初始化失败: %d", rc);
}

/** @brief 协议栈复位时丢弃未完成传输，避免旧分包污染新连接。 */
void onReset(int reason) {
    clearIncoming();
    readOffset = 0;
    saved = false;
    status = "idle";
    ESP_LOGW(TAG, "蓝牙协议栈复位: %d", reason);
}

/** @brief 运行 NimBLE 主机任务，退出时释放 FreeRTOS 包装资源。 */
void hostTask(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

esp_err_t BleConfigService::start(const AppConfig &config) {
    configJson = config.toJson();
    if (configJson.empty()) return ESP_ERR_NO_MEM;
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_BT);
    if (err != ESP_OK) return err;
    std::snprintf(deviceName, sizeof(deviceName), "ESP32S3-Config-%02X%02X%02X",
                  mac[3], mac[4], mac[5]);
    err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(services.data());
    if (rc == 0) rc = ble_gatts_add_svcs(services.data());
    if (rc == 0) rc = ble_svc_gap_device_name_set(deviceName);
    if (rc != 0) {
        nimble_port_deinit();
        return ESP_FAIL;
    }
    ble_store_config_init();
    ESP_LOGI(TAG, "蓝牙配网服务: %s，设备配对码: %06lu，请妥善保管",
             deviceName, static_cast<unsigned long>(passkey));
    nimble_port_freertos_init(hostTask);
    return ESP_OK;
}

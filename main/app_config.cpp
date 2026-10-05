#include "app_config.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>

static const char *TAG = "CONFIG";

namespace {

constexpr const char *CONFIG_PATH = "/spiffs/wifi.json";
constexpr const char *TEMP_PATH = "/spiffs/wifi.tmp";
constexpr const char *NVS_NAMESPACE = "wifi_config";
constexpr size_t MAX_CONFIG_SIZE = 1024;
bool spiffsMounted = false;
using Json = nlohmann::ordered_json;
constexpr std::array<const char *, 5> CONFIG_KEYS = {
    "ssid", "password", "static_ip", "gateway", "netmask"};

/** @brief 尝试挂载 SPIFFS，禁止自动格式化，挂载失败时仍允许通过 NVS 配网。 */
void mountSpiffs() {
    if (spiffsMounted)
        return;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    spiffsMounted = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    if (!spiffsMounted)
        ESP_LOGE(TAG, "配置文件系统挂载失败: %s", esp_err_to_name(err));
}

/** @brief 读取有长度上限的配置文件，不把异常大文件加载到内存。 */
std::string readFile() {
    FILE *file = std::fopen(CONFIG_PATH, "rb");
    if (!file)
        return {};
    std::array<char, MAX_CONFIG_SIZE + 1> buffer{};
    const size_t size = std::fread(buffer.data(), 1, buffer.size(), file);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed || size > MAX_CONFIG_SIZE)
        return {};
    return std::string(buffer.data(), size);
}

/** @brief 读取蓝牙配网保存的 NVS 配置副本。 */
std::string readNvs() {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return {};
    size_t size = 0;
    std::string json;
    if (nvs_get_str(handle, "json", nullptr, &size) == ESP_OK && size > 1 &&
        size <= MAX_CONFIG_SIZE + 1) {
        json.resize(size);
        if (nvs_get_str(handle, "json", json.data(), &size) == ESP_OK) {
            json.resize(size - 1);
        } else {
            json.clear();
        }
    }
    nvs_close(handle);
    return json;
}

/** @brief 配置只允许扁平对象，限制 JSON 嵌套深度以保护 BLE 主机任务栈。 */
bool isFlatJson(const std::string &json) {
    bool quoted = false;
    bool escaped = false;
    int depth = 0;
    for (char ch : json) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == '[' || ch == ']') {
            return false;
        } else if (ch == '{') {
            if (++depth > 1)
                return false;
        } else if (ch == '}') {
            if (--depth < 0)
                return false;
        }
    }
    return !quoted && depth == 0;
}

/** @brief 解析严格的点分十进制 IPv4，不接受缩写、空白或尾随字符。 */
bool parseIpv4(const std::string &text, uint32_t &address) {
    address = 0;
    size_t offset = 0;
    for (int part = 0; part < 4; ++part) {
        unsigned value = 0;
        size_t digits = 0;
        while (offset < text.size() && text[offset] >= '0' &&
               text[offset] <= '9') {
            value = value * 10 + static_cast<unsigned>(text[offset++] - '0');
            if (++digits > 3 || value > 255)
                return false;
        }
        if (!digits)
            return false;
        address = (address << 8) | value;
        if (part < 3 && (offset >= text.size() || text[offset++] != '.'))
            return false;
    }
    return offset == text.size();
}

/** @brief 序列化完整配置，正确转义 SSID 与密码中的特殊字符。 */
std::string serialize(const AppConfig &config) {
    const Json root = {{"ssid", config.ssid},
                       {"password", config.password},
                       {"static_ip", config.staticIp},
                       {"gateway", config.gateway},
                       {"netmask", config.netmask}};
    // 禁用异常时也不能因非法 UTF-8 中止任务；保存前会核对是否发生替换。
    return root.dump(-1, ' ', false, Json::error_handler_t::replace);
}

/** @brief 通过临时文件同步配置；NVS 副本用于弥补 SPIFFS 非事务性写入。 */
bool syncFile(const std::string &json) {
    if (!spiffsMounted)
        return false;
    FILE *file = std::fopen(TEMP_PATH, "wb");
    if (!file)
        return false;
    bool ok = std::fwrite(json.data(), 1, json.size(), file) == json.size();
    if (ok && std::fflush(file) != 0)
        ok = false;
    if (ok && fsync(fileno(file)) != 0)
        ok = false;
    if (std::fclose(file) != 0)
        ok = false;
    if (ok) {
        // SPIFFS 不支持覆盖重命名，NVS 已先提交，缺失文件可在下次启动恢复。
        if (std::remove(CONFIG_PATH) != 0 && errno != ENOENT)
            ok = false;
        if (ok && std::rename(TEMP_PATH, CONFIG_PATH) != 0)
            ok = false;
    }
    if (!ok)
        std::remove(TEMP_PATH);
    return ok;
}

} // namespace

bool AppConfig::parse(const std::string &json, AppConfig &config,
                      std::string &error) {
    if (json.empty() || json.size() > MAX_CONFIG_SIZE ||
        json.find('\0') != std::string::npos ||
        json.find("\\u0000") != std::string::npos) {
        error = "配置为空、过长或包含空字符";
        return false;
    }
    if (!isFlatJson(json)) {
        error = "配置必须为扁平 JSON 对象";
        return false;
    }
    // DOM 会覆盖同名键，在解析回调中统计必填字段以保留重复字段检测。
    std::array<size_t, CONFIG_KEYS.size()> counts{};
    const auto countFields = [&counts](int, Json::parse_event_t event,
                                       Json &value) {
        if (event == Json::parse_event_t::key) {
            const auto &key = value.get_ref<const Json::string_t &>();
            for (size_t i = 0; i < CONFIG_KEYS.size(); ++i) {
                if (key == CONFIG_KEYS[i])
                    ++counts[i];
            }
        }
        return true;
    };
    const Json root = Json::parse(json, countFields, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "配置不是合法 JSON 对象";
        return false;
    }
    AppConfig candidate;
    std::string *values[] = {&candidate.ssid, &candidate.password,
                             &candidate.staticIp, &candidate.gateway,
                             &candidate.netmask};
    for (size_t i = 0; i < CONFIG_KEYS.size(); ++i) {
        const auto item = root.find(CONFIG_KEYS[i]);
        if (item == root.end() || !item->is_string()) {
            error = std::string("缺少字符串字段: ") + CONFIG_KEYS[i];
            return false;
        }
        if (counts[i] != 1) {
            error = std::string("字段重复: ") + CONFIG_KEYS[i];
            return false;
        }
        *values[i] = item->get_ref<const Json::string_t &>();
        if (values[i]->find('\0') != std::string::npos) {
            error = "配置包含空字符";
            return false;
        }
    }
    if (candidate.ssid.empty() || candidate.ssid.size() > 32) {
        error = "SSID 长度必须为 1 至 32 字节";
        return false;
    }
    const size_t length = candidate.password.size();
    const bool hexPsk =
        length == 64 && candidate.password.find_first_not_of(
                            "0123456789abcdefABCDEF") == std::string::npos;
    if (!(length == 0 || (length >= 8 && length <= 63) || hexPsk)) {
        error = "密码必须为空、8 至 63 字节或 64 位十六进制 PSK";
        return false;
    }
    uint32_t ip, gateway, mask;
    if (!parseIpv4(candidate.staticIp, ip) ||
        !parseIpv4(candidate.gateway, gateway) ||
        !parseIpv4(candidate.netmask, mask)) {
        error = "IP、网关或子网掩码格式错误";
        return false;
    }
    const uint32_t host = ~mask;
    if (host < 3 || mask == 0 || (host & (host + 1)) != 0 || (ip & host) == 0 ||
        (ip & host) == host || (gateway & host) == 0 ||
        (gateway & host) == host || (ip & mask) != (gateway & mask) ||
        ip == gateway || (ip >> 24) == 0 || (ip >> 24) == 127 ||
        (ip >> 24) >= 224 || (gateway >> 24) == 0 || (gateway >> 24) == 127 ||
        (gateway >> 24) >= 224) {
        error = "子网掩码、主机地址或同网段网关无效";
        return false;
    }
    config = candidate;
    error.clear();
    return true;
}

AppConfig AppConfig::load() {
    mountSpiffs();
    AppConfig config;
    std::string error;
    const std::string stored = readNvs();
    if (!stored.empty() && parse(stored, config, error)) {
        if (spiffsMounted && readFile() != stored && !syncFile(stored)) {
            ESP_LOGW(TAG, "配置文件同步失败，使用 NVS 配置副本");
        }
    } else if (!parse(readFile(), config, error)) {
        ESP_LOGW(TAG, "网络配置不可用，请通过蓝牙配网: %s", error.c_str());
        return config;
    }
    ESP_LOGI(TAG, "配置加载完成: ssid=%s ip=%s gw=%s", config.ssid.c_str(),
             config.staticIp.c_str(), config.gateway.c_str());
    return config;
}

esp_err_t AppConfig::save() const {
    const std::string json = serialize(*this);
    if (json.empty())
        return ESP_ERR_NO_MEM;
    AppConfig validated;
    std::string error;
    if (!parse(json, validated, error) || validated.ssid != ssid ||
        validated.password != password || validated.staticIp != staticIp ||
        validated.gateway != gateway || validated.netmask != netmask) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_set_str(handle, "json", json.c_str());
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK)
        return err;
    if (!syncFile(json))
        ESP_LOGW(TAG, "配置已持久保存，wifi.json 同步失败，下次启动重试");
    ESP_LOGI(TAG, "网络配置已保存，重启后生效");
    return ESP_OK;
}

std::string AppConfig::toJson() const { return serialize(*this); }

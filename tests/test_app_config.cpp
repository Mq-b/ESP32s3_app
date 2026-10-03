#ifdef _WIN32
#include <io.h>
// 主机测试使用 Windows 的等价文件同步接口，不修改固件实现。
#define fsync _commit
#endif

#include "app_config.cpp"

#include <cstdlib>
#include <iostream>

namespace {

/** @brief 校验断言，失败时输出不含网络凭据的测试说明并退出。 */
void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

/** @brief 生成包含中文及 JSON 转义字符的有效配置。 */
AppConfig validConfig() {
    AppConfig config;
    config.ssid = "测试网络";
    config.password = "test\"\\pass";
    return config;
}

/** @brief 验证非法输入被拒绝，同时保留调用方原有配置。 */
void reject(const std::string &text) {
    AppConfig config = validConfig();
    const std::string before = serialize(config);
    std::string error;
    check(!AppConfig::parse(text, config, error), "非法配置未被拒绝");
    check(!error.empty(), "失败时未提供错误说明");
    check(serialize(config) == before, "解析失败修改了原配置");
}

}  // namespace

/** @brief 在无 C++ 异常的主机环境运行固件配置解析与序列化回归测试。 */
int main() {
    const AppConfig original = validConfig();
    const std::string text = serialize(original);
    AppConfig parsed;
    std::string error = "旧错误";
    check(AppConfig::parse(text, parsed, error), "完整配置解析失败");
    check(error.empty(), "成功时未清理错误");
    check(serialize(parsed) == text, "中文或特殊字符往返不一致");
    check(text.starts_with("{\"ssid\":"), "序列化字段顺序改变");
    check(AppConfig::parse(" \n" + text + "\t ", parsed, error), "合法尾随空白被拒绝");

    for (const std::string &invalid : std::array<std::string, 8>{"", "{", "null", "[]", "{}", text + "junk",
                                     text + text, std::string(1025, ' ')}) reject(invalid);
    reject(text + std::string(1, '\0'));
    for (const char *key : CONFIG_KEYS) {
        Json root = Json::parse(text);
        root.erase(key);
        reject(root.dump());
        for (const Json &value : {Json(nullptr), Json(42), Json(true), Json::array(), Json::object()}) {
            root = Json::parse(text);
            root[key] = value;
            reject(root.dump());
        }
        reject(text.substr(0, text.size() - 1) + ",\"" + key + "\":\"duplicate\"}");
    }
    reject(text.substr(0, text.size() - 1) + R"(,"\u0073sid":"duplicate"})");
    reject(text.substr(0, text.size() - 1) + R"(,"extra":{})");
    reject(text.substr(0, text.size() - 1) + R"(,"extra":[]})");
    reject(R"({"ssid":"bad\u0000ssid","password":"","static_ip":"192.168.0.10","gateway":"192.168.0.1","netmask":"255.255.255.0"})");
    std::string malformed = text;
    malformed.insert(malformed.find("测试"), 1, static_cast<char>(0xFF));
    reject(malformed);

    Json extra = Json::parse(text);
    extra["extra"] = "兼容未知字段";
    check(AppConfig::parse(extra.dump(), parsed, error), "未知扁平字段被拒绝");
    extra["ssid"] = std::string(32, 's');
    check(AppConfig::parse(extra.dump(), parsed, error), "32 字节 SSID 被拒绝");
    extra["ssid"] = std::string(33, 's');
    reject(extra.dump());
    extra["ssid"] = "";
    reject(extra.dump());
    for (const std::string &password : std::array<std::string, 4>{"", "12345678", std::string(63, 'p'), std::string(64, 'a')}) {
        extra = Json::parse(text);
        extra["password"] = password;
        check(AppConfig::parse(extra.dump(), parsed, error), "合法密码被拒绝");
    }
    for (const std::string &password : std::array<std::string, 3>{"short", std::string(64, 'g'), std::string(65, 'a')}) {
        extra["password"] = password;
        reject(extra.dump());
    }
    for (const auto &[key, value] : std::array<std::pair<const char *, const char *>, 7>{{
             {"static_ip", "1.2.3"}, {"static_ip", "192.168.0.0"},
             {"static_ip", "192.168.0.255"}, {"static_ip", "192.168.0.1"},
             {"gateway", "192.168.1.1"}, {"netmask", "255.0.255.0"},
             {"netmask", "255.255.255.254"}}}) {
        extra = Json::parse(text);
        extra[key] = value;
        reject(extra.dump());
    }
    extra = Json::parse(text);
    extra["extra"] = "";
    extra["extra"] = std::string(1024 - extra.dump().size(), 'x');
    check(extra.dump().size() == 1024, "长度边界测试构造失败");
    check(AppConfig::parse(extra.dump(), parsed, error), "1024 字节配置被拒绝");
    extra["extra"].get_ref<Json::string_t &>() += 'x';
    reject(extra.dump());

    AppConfig malformedConfig = original;
    malformedConfig.ssid = std::string(1, static_cast<char>(0xFF));
    check(malformedConfig.save() == ESP_ERR_INVALID_ARG, "非法 UTF-8 保存未被拒绝");
    malformedConfig = original;
    malformedConfig.password += '\0';
    check(malformedConfig.save() == ESP_ERR_INVALID_ARG, "空字符保存未被拒绝");
    std::cout << "配置 JSON 回归测试通过\n";
}

# ZXing ESP-IDF 适配

上游：https://github.com/zxing-cpp/zxing-cpp
版本：v2.3.0，提交 d6068bcebeb8fd9f0d35a99b00d202be86a14dbe。
仅复制 core 和 LICENSE，未修改上游 C/C++ 源码；core/CMakeLists.txt 增加 ESP_PLATFORM 条件，跳过桌面安装导出规则。禁用编码器、C API 和预编译头。
启用 CONFIG_COMPILER_CXX_EXCEPTIONS，以便恢复输入和内存异常。

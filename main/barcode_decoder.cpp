#include "barcode_decoder.h"
#include "BarcodeFormat.h"
#include "ImageView.h"
#include "ReadBarcode.h"
#include "ReaderOptions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "barcode_runtime.h"
#include "stb_image.h"
#include <memory>
#include <new>
#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::ordered_json;
constexpr size_t MAX_JPEG_BYTES = 1024 * 1024;
constexpr int MAX_IMAGE_DIMENSION = 4096;
constexpr const char *TAG = "BARCODE";

/** @brief 管理当前解码任务的协作调度作用域，不降低任务优先级。 */
class DecodeScheduleGuard {
public:
    /** @brief 开始协作调度计时。 */
    DecodeScheduleGuard() { barcode_runtime_begin(); }
    /** @brief 结束计时并输出让出统计。 */
    ~DecodeScheduleGuard() { barcode_runtime_end(); }
    DecodeScheduleGuard(const DecodeScheduleGuard &) = delete;
    DecodeScheduleGuard &operator=(const DecodeScheduleGuard &) = delete;
};

/** @brief 替换不可信码文本中的非法 UTF-8，避免序列化异常。 */
std::string serialize(const Json &value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

/** @brief 构造含帧编号的错误响应。 */
BarcodeScanResult error(int status, uint32_t frameId, const char *message) {
    return {status, serialize({{"ok", false}, {"frameId", frameId}, {"error", message}})};
}
}  // namespace

BarcodeScanResult BarcodeDecoder::scan(const uint8_t *data, size_t size, const char *path, uint32_t frameId) {
    try {
        if (size == 0 || size > MAX_JPEG_BYTES || (!path && !data))
            return error(400, frameId, "JPEG 输入为空或超过 1 MiB");
        const int64_t start = esp_timer_get_time();
        int width = 0, height = 0, channels = 0;
        const bool valid = path ? stbi_info(path, &width, &height, &channels)
                                : stbi_info_from_memory(data, static_cast<int>(size), &width, &height, &channels);
        if (!valid || width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION ||
            (channels != 1 && channels != 3))
            return error(400, frameId, "仅接受单边不超过 4096 像素的灰度或 RGB JPEG");

        DecodeScheduleGuard scheduleGuard;
        // 原始灰度图直接交给 ZXing，不做额外采样，保留小条码细节。
        using ImagePtr = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>;
        ImagePtr image(path ? stbi_load(path, &width, &height, &channels, 1)
                            : stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 1),
                       &stbi_image_free);
        if (!image) return error(422, frameId, "JPEG 解码失败或工作区分配失败");
        const int64_t decoded = esp_timer_get_time();
        ESP_LOGI(TAG, "开始识别，帧 %lu，原图 %d×%d",
                 static_cast<unsigned long>(frameId), width, height);
        const auto options = ZXing::ReaderOptions()
            .setTryHarder(true).setTryRotate(true).setTryInvert(true).setTryDownscale(true)
            .setMaxNumberOfSymbols(4);
        const ZXing::ImageView view(image.get(), width, height, ZXing::ImageFormat::Lum, width);
        auto commonOptions = options;
        commonOptions.setFormats(ZXing::BarcodeFormat::Any);
        auto barcodes = ZXing::ReadBarcodes(view, commonOptions);
        const int64_t finished = esp_timer_get_time();
        Json codes = Json::array();
        for (const auto &barcode : barcodes) {
            if (!barcode.isValid()) continue;
            Json corners = Json::array();
            for (const auto &point : barcode.position()) corners.push_back({{"x", point.x}, {"y", point.y}});
            codes.push_back({{"format", ZXing::ToString(barcode.format())}, {"text", barcode.text()}, {"corners", corners}});
        }
        ESP_LOGI(TAG, "识别完成，帧 %lu，结果 %u，JPEG %.1f ms，ZXing %.1f ms",
                 static_cast<unsigned long>(frameId), static_cast<unsigned>(codes.size()),
                 (decoded - start) / 1000.0, (finished - decoded) / 1000.0);
        return {200, serialize({{"ok", true}, {"frameId", frameId}, {"width", width}, {"height", height},
                               {"decodeMs", (decoded - start) / 1000.0},
                               {"scanMs", (finished - decoded) / 1000.0}, {"codes", codes}})};
    } catch (const std::bad_alloc &) {
        return error(503, frameId, "扫码工作区内存不足，请检查 PSRAM");
    } catch (const std::exception &) {
        return error(422, frameId, "图片或二维码解码异常");
    }
}

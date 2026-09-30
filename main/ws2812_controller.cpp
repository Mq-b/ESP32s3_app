#include "ws2812_controller.h"

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr uint32_t RMT_RESOLUTION_HZ = 10 * 1000 * 1000;
constexpr uint32_t MIN_BLINK_INTERVAL_MS = 10;
}

Ws2812Controller::Ws2812Controller(int gpioNum) {
    const led_strip_config_t stripConfig = {
        .strip_gpio_num = gpioNum,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    const led_strip_rmt_config_t rmtConfig = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 0,
        .flags = {
            .with_dma = false,
        },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&stripConfig, &rmtConfig, &strip_));
    ESP_ERROR_CHECK(led_strip_clear(strip_));

    BaseType_t result = xTaskCreate(taskEntry, "ws2812", 3072, this, 4, nullptr);
    ESP_ERROR_CHECK(result == pdPASS ? ESP_OK : ESP_FAIL);
}

void Ws2812Controller::turnOn(Ws2812Color color) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = Ws2812Mode::On;
    color_ = color;
    applyColorLocked(color_);
}

void Ws2812Controller::turnOff() {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = Ws2812Mode::Off;
    turnOffLocked();
}

void Ws2812Controller::startBlinking(Ws2812Color color, uint32_t onMs, uint32_t offMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    color_ = color;
    onMs_ = onMs < MIN_BLINK_INTERVAL_MS ? MIN_BLINK_INTERVAL_MS : onMs;
    offMs_ = offMs < MIN_BLINK_INTERVAL_MS ? MIN_BLINK_INTERVAL_MS : offMs;
    mode_ = Ws2812Mode::Blink;
    applyColorLocked(color_);
}

Ws2812Mode Ws2812Controller::mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

void Ws2812Controller::taskEntry(void *arg) {
    static_cast<Ws2812Controller *>(arg)->taskLoop();
}

void Ws2812Controller::taskLoop() {
    bool wasBlinking = false;
    bool isOn = false;
    while (true) {
        uint32_t delayMs = 100;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (mode_ == Ws2812Mode::Blink) {
                isOn = wasBlinking ? !isOn : true;
                wasBlinking = true;
                if (isOn) {
                    applyColorLocked(color_);
                    delayMs = onMs_;
                } else {
                    turnOffLocked();
                    delayMs = offMs_;
                }
            } else {
                wasBlinking = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(delayMs));
    }
}

void Ws2812Controller::applyColorLocked(Ws2812Color color) {
    ESP_ERROR_CHECK(led_strip_set_pixel(strip_, 0, color.red, color.green, color.blue));
    ESP_ERROR_CHECK(led_strip_refresh(strip_));
}

void Ws2812Controller::turnOffLocked() {
    ESP_ERROR_CHECK(led_strip_clear(strip_));
}

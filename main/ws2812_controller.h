#pragma once

#include "led_strip.h"

#include <cstdint>
#include <mutex>

/**
 * @brief WS2812 灯珠的工作模式。
 */
enum class Ws2812Mode {
    /** @brief 熄灭。 */
    Off,
    /** @brief 常亮。 */
    On,
    /** @brief 周期闪烁。 */
    Blink,
};

/**
 * @brief RGB 颜色值。
 */
struct Ws2812Color {
    /** @brief 红色通道亮度，范围为 0 至 255。 */
    uint8_t red;
    /** @brief 绿色通道亮度，范围为 0 至 255。 */
    uint8_t green;
    /** @brief 蓝色通道亮度，范围为 0 至 255。 */
    uint8_t blue;
};

/**
 * @brief 控制板载单颗 WS2812 RGB 灯珠。
 * @note 初始化后会创建一个后台任务；该对象应在应用生命周期内保持有效。
 */
class Ws2812Controller {
public:
    /**
     * @brief 初始化 WS2812 控制器。
     * @param gpioNum WS2812 数据线连接的 GPIO 编号。
     * @note 本开发板的板载 WS2812 数据线为 GPIO48。
     */
    explicit Ws2812Controller(int gpioNum);

    /**
     * @brief 设置灯珠为指定颜色的常亮状态。
     * @param color 常亮时显示的 RGB 颜色。
     */
    void turnOn(Ws2812Color color = {16, 16, 16});

    /**
     * @brief 关闭灯珠。
     */
    void turnOff();

    /**
     * @brief 设置灯珠按指定周期闪烁。
     * @param color 闪烁时点亮的 RGB 颜色。
     * @param onMs 每次点亮持续时间，单位为毫秒，必须大于 0。
     * @param offMs 每次熄灭持续时间，单位为毫秒，必须大于 0。
     */
    void startBlinking(Ws2812Color color = {16, 16, 16}, uint32_t onMs = 500,
                       uint32_t offMs = 500);

    /**
     * @brief 获取当前灯珠工作模式。
     * @return 当前工作模式。
     */
    Ws2812Mode mode() const;

    /**
     * @brief 设置当前 RGB 颜色。
     * @param color 要保存的 RGB 颜色；常亮或闪烁模式下会立即应用。
     */
    void setColor(Ws2812Color color);

    /**
     * @brief 获取当前 RGB 颜色。
     * @return 当前保存的 RGB 颜色。
     */
    Ws2812Color color() const;

private:
    static void taskEntry(void *arg);
    void taskLoop();
    void applyColorLocked(Ws2812Color color);
    void turnOffLocked();

    led_strip_handle_t strip_ = nullptr;
    mutable std::mutex mutex_;
    Ws2812Mode mode_ = Ws2812Mode::Off;
    Ws2812Color color_ {16, 16, 16};
    uint32_t onMs_ = 500;
    uint32_t offMs_ = 500;
};

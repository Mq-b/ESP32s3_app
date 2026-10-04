#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
/** @brief 开始当前任务的协作调度计时，仅允许串行解码任务调用。 */
void barcode_runtime_begin(void);
/** @brief 结束当前任务的协作调度，输出计算与让出时间统计。 */
void barcode_runtime_end(void);
/**
 * @brief 在计算循环中检查时间预算，累计计算 50ms 后阻塞一个 tick。
 * @note 仅作用于 begin 指定的任务，不改变其他任务优先级或看门狗配置。
 */
void barcode_runtime_checkpoint(void);
/** @brief 热循环每 64 次调用检查预算，减少频繁读取微秒时钟的开销。
 * @note 本地图像库仅由一个串行解码任务使用。
 */
static inline void barcode_runtime_poll(void) {
    static unsigned calls = 0;
    if ((++calls & 63U) == 0) barcode_runtime_checkpoint();
}
#else
/** @brief 主机测试不操作 FreeRTOS 调度。 */
static inline void barcode_runtime_begin(void) {}
/** @brief 主机测试不操作 FreeRTOS 调度。 */
static inline void barcode_runtime_end(void) {}
/** @brief 主机测试不操作 FreeRTOS 调度。 */
static inline void barcode_runtime_checkpoint(void) {}
/** @brief 主机测试不操作 FreeRTOS 调度。 */
static inline void barcode_runtime_poll(void) {}
#endif

#ifdef __cplusplus
}
#endif

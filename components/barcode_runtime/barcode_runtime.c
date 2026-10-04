#include "barcode_runtime.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t active_task;
static int64_t last_yield;
static int64_t yield_us;
static unsigned yield_count;

void barcode_runtime_begin(void) {
    active_task = xTaskGetCurrentTaskHandle();
    last_yield = esp_timer_get_time();
    yield_us = 0;
    yield_count = 0;
}

void barcode_runtime_checkpoint(void) {
    if (active_task != xTaskGetCurrentTaskHandle()) return;
    const int64_t now = esp_timer_get_time();
    if (now - last_yield < 50000) return;
    vTaskDelay(1);
    last_yield = esp_timer_get_time();
    yield_us += last_yield - now;
    ++yield_count;
}

void barcode_runtime_end(void) {
    ESP_LOGI("BARCODE_SCHED", "解码核 %d，优先级 %u，让出 %u 次，阻塞合计 %.1f ms",
             xPortGetCoreID(), (unsigned)uxTaskPriorityGet(NULL), yield_count, yield_us / 1000.0);
    active_task = NULL;
}

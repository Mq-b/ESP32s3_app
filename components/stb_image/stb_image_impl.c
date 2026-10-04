#include "esp_heap_caps.h"
#include <stddef.h>

/**
 * @brief 优先从 PSRAM 分配图像内存，失败时尝试内部堆。
 * @param size 所需字节数。
 * @return 缓冲区地址，失败返回空指针。
 */
static void *image_alloc(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

/**
 * @brief 调整 JPEG 工作缓冲大小，优先使用 PSRAM。
 * @param p 原缓冲区，可为空。
 * @param size 新的字节数。
 * @return 新地址，失败时原缓冲仍有效。
 */
static void *image_realloc(void *p, size_t size) {
    return heap_caps_realloc(p, size, MALLOC_CAP_8BIT);
}

#define STBI_ONLY_JPEG
#define STBI_NO_SIMD
#define STBI_MALLOC(size) image_alloc(size)
#define STBI_REALLOC(p, size) image_realloc(p, size)
#define STBI_FREE(p) heap_caps_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

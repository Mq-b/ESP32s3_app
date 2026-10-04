# stb_image ESP-IDF 适配

上游：https://github.com/nothings/stb
提交：2c980bb59875b0d32144a71867fbdebb2f77cd20。
保留原始 stb_image.h 和 LICENSE。仅启用 JPEG，最大单边 640；自定义分配优先使用 PSRAM。
保留文件解码入口，以便 Flash 临时缓存不必整文件读回 RAM。

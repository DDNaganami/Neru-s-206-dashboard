// 最小 lv_conf.h:未定义的选项全部走 LVGL 内部默认值。
// 以后需要自定义字体/动画/主题开关时,往这里加即可。
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

// esp32dev(无 PSRAM)内存紧张:堆 48KB。
// 换 ESP32-S3 N16R8(8MB PSRAM)后调回 64K+,并把显示缓冲放进 PSRAM。
#define LV_MEM_SIZE (48U * 1024U)

// 调试开关(联调时按需打开)
#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0

#endif

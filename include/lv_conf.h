// 最小 lv_conf.h:未定义的选项全部走 LVGL 内部默认值。
// 以后需要自定义字体/动画/主题开关时,往这里加即可。
#ifndef LV_CONF_H
#define LV_CONF_H

// 颜色深度:设备 16 位;宿主机预览(env:pcpreview)用 -DLV_COLOR_DEPTH=32 覆盖 ——
// 16 位时任何 opa<255 的大对象(整屏淡入、半透明轨道)都要开 ARGB8888 离屏层,
// 而层从显示缓冲里切,装不下整屏 → 下半屏回绕到顶部(已在预览上踩过,别改回 16)。
#ifndef LV_COLOR_DEPTH
#define LV_COLOR_DEPTH 16
#endif

// esp32dev(无 PSRAM)内存紧张:堆 48KB。
// 换 ESP32-S3 N16R8(8MB PSRAM)后调回 64K+,并把显示缓冲放进 PSRAM。
// 宿主机预览(env:pcpreview)通过 -DLV_MEM_SIZE=262144 覆盖此值。
#ifndef LV_MEM_SIZE
#define LV_MEM_SIZE (48U * 1024U)
#endif

// 调试开关(联调时按需打开)
#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0

// ------------------------------------------------------------
// 字体:仪表要显示数字和单位,而 LVGL 默认只开 14 号
// ------------------------------------------------------------
// 用途:
//   48 号 —— 转速/速度的**大数字**(表盘正上方)
//   18 号 —— 单位(km/h、rpm)与水温数字
//   14 号 —— LVGL 默认字体,留着给将来的小字
// 代价:每个字号是一份点阵数据,会占 Flash。当前 app 槽 1MB、
//   固件只占 51%,余量足够;若以后紧了,优先砍 48 号换更小的字号。
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_48 1

// 宿主机预览:断言失败打印位置并退出,而不是静默死循环
// (LVGL 的 LV_ASSERT_* 把 LV_ASSERT_HANDLER 直接嵌在 do-while 里、后面不带分号,
//  所以这里必须自带结尾分号)
#if defined(DASH_DISPLAY_PREVIEW)
#define LV_ASSERT_HANDLER_INCLUDE <stdio.h>
#define LV_ASSERT_HANDLER \
  do { \
    printf("LVGL assert %s:%d\n", __FILE__, __LINE__); \
    fflush(stdout); \
    extern void abort(void); \
    abort(); \
  } while (0);
#endif

#endif

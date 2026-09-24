#pragma once
#include <lvgl.h>

// 显示驱动接口:注册两块 480×480 圆屏并返回。
// 当前为无屏桩驱动(DASH_DISPLAY_STUB=1):渲染进缓冲后丢弃,只用于逻辑联调。
// 屏到货后:在 dash_display.cpp 里加实驱动(ST7701S SPI / LovyanGFX),去掉桩。
void dash_display_init();
lv_display_t* dash_display_left();
lv_display_t* dash_display_right();
// 每个主循环调一次:设备上为空;pcpreview 里落 BMP 帧(见 preview/preview.html)
void dash_display_poll();

// ---- 只给 pcpreview 用的三个小接口（**只有预览那一支有定义**，见 dash_display.cpp）
//
// ★ 为什么帧号要暴露出来（2026-09-24 新增）：告警那一拍的"屏上闪"是跟着
//   蜂鸣器走的（`Alerts::beeping()` 只持续 `beep_ms` = 120ms），而预览**每
//   200ms 才落一帧** ⇒ 有一半的落帧时刻那一拍已经过去了，"按了 O 却没看见闪"
//   就会出现。修法是让主循环知道"这一拍有没有被某一帧拍到"，而它必须能问
//   显示侧**落了第几帧** —— 于是有了这个 getter（设备端连声明都不用：
//   调用点整个在 `#if defined(DASH_DISPLAY_PREVIEW)` 里）。
//
// ★ 遮罩开关（`V` 键）：默认开。关掉之后落盘的是**裸画布**，
//   用于"四角里到底画了什么"那种排查。
#if defined(DASH_DISPLAY_PREVIEW)
uint32_t dash_display_preview_frames(void);
void dash_display_preview_set_panel_mask(bool on);
bool dash_display_preview_panel_mask(void);
#endif

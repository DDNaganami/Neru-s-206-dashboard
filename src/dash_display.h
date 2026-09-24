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

// ---- 只给 `[env:esp32s3-rgb]`（真屏那一份构建）用的蜂鸣器出口
//
// ★ 为什么声明放在**这个**头里、实现放在 `src/dash_display_rgb.cpp`：
//   2.8C 板载蜂鸣器挂在 **TCA9554（I2C 扩展器）的 EXIO8** 上，而那颗芯片的
//   输出寄存器里**同时挂着 `LCD_RST`(EXIO1) 与 `LCD_CS`(EXIO3)**
//   ⇒ "影子寄存器 + 读-改-写"只能有**一份**，就是显示驱动里那一份。
//   在这里另写一遍 I2C 时序 ⇒ 两份影子互相覆盖丢位 ⇒ 丢到 RST/CS 上就是
//   一次**面板复位**（而面板复位**不会自己回来**）。详见
//   `docs/RGB-PANEL-2.8C.md` §13.7 与 `src/dash_display_rgb.cpp` 里那个函数的注释。
//
// ★ 门用的是**既有的** `DASH_DISPLAY_RGB` 宏（`platformio.ini` 里早就有、
//   且**只有** `[env:esp32s3-rgb]` 定义它）⇒ 其余 env（抓帧盒 `esp32s3` /
//   `esp32dev` / `-vaninv` / `-vansniff` / `esp32s3-spi` / `esp32s3-linkloop`）
//   的编译单元里**这个声明不存在、调用点也不存在**，行为逐字节不变 ✓。
//   ★ **没有新增任何 `-D`**（能不新增就不新增）。
//
// ★ 返回 void：调用方（`BuzzerExio` 的序列）不需要知道成没成 ——
//   "写没写对"由驱动自己那行 `buzz: exio 0xXX -> 0xXX (mask 0x01…)` 自证。
#if defined(DASH_DISPLAY_RGB)
void dash_buzzer_set(bool on, void* ctx);
#endif

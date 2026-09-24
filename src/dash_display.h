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

// ---- 只给 `[env:esp32s3-rgb]`（真屏那一份构建）用的**面板健康守护**出口
//
// ★ 起因 = 产品要求（业主原话）："**上实车的时候可不能这样，这毕竟是仪表盘，要常亮的**"。
//   2026-09-24 当晚真发生过"**屏黑了、固件一直活着**"（串口上 `vsync` 照涨、
//   `timeout=0`、每秒一行 `206 dash ok`），复位一次就恢复。
//   ⇒ 三层防线的第 ② 层（可检测故障自愈）+ 第 ③ 层（现场恢复路径）落在这里：
//
//   · `dash_panel_guard_*`：守护的**读数**（诊断页那行 `guard rd_ok=… fix=… bl=…`）。
//     判据/周期/计数全在 `lib/dashcore/panel_guard.*`（宿主机逐条钉着），
//     这里只把四个注入回调接上真硬件。
//   · `dash_display_panel_reinit()`：**重跑 ST7701 的初始化序列 + 重发当前 framebuffer**
//     —— 串口命令 `r` 与守护的自动恢复都走这一条。
//     ★ 它**不碰** PCLK / bounce / num_fbs / 引脚（那些是"不要做"清单上的东西）。
//
// ★ 与蜂鸣器那一节同一条理由：门用**既有的** `DASH_DISPLAY_RGB`（`platformio.ini` 里
//   早就有、且只有 `[env:esp32s3-rgb]` 定义它）⇒ 抓帧盒那几个 env 的编译单元里
//   **这些声明一个都不存在**，行为逐字节不变 ✓。**没有新增任何 `-D`** ✓。
#if defined(DASH_DISPLAY_RGB)
// 面板重初始化（幂等；未初始化过时返回 false，一个字节都不动）。
// 返回 true = 真的重跑了初始化 + 重发了当前画面。
bool dash_display_panel_reinit(const char* why);
// 守护的四个读数（诊断页/日志用；没上线时全 0）。
uint32_t dash_panel_guard_rd_ok(void);
uint32_t dash_panel_guard_fix(void);
uint32_t dash_panel_guard_bl(void);
uint32_t dash_panel_guard_anomalies(void);
// ★ 临时故障注入（**默认构建里永远是 0**，见 .cpp 里那一段）：
//   把扩展器输出寄存器的某一位**故意写错**，用来在真机上验守护能不能发现并修回来。
//   0 = 不注入。测完这条路径整个删掉（它只在 `-DPANEL_GUARD_FAULT_INJECT=1` 里存在）。
void dash_panel_guard_fault_inject(uint8_t bit);
#endif

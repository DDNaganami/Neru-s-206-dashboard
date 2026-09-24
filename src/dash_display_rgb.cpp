// ============================================================
// 真实 RGB 并口屏驱动(480×480,ST7701)—— **双 framebuffer + vsync 边界换帧**
//
// 编译开关:`-DDASH_DISPLAY_RGB=1`(见 platformio.ini 的 [env:esp32s3-rgb])。
// 与桩驱动/预览驱动共用同一个接口(dash_display.h 的三个函数),所以
// dash_ui.cpp 一行都不用改 —— 这正是当初把它抽成接口的目的。
//
// ------------------------------------------------------------
// ★★ 2026-09-24:这一版是**换栈之后**的驱动(撕裂的根治)
//
// 上一轮把撕裂的**结构性根因**查清了(记录在 docs/RGB-PANEL-2.8C.md 第 5/9 节):
//   旧栈(官方 espressif32 7.1.3 = arduino-esp32 **2.0.17** / IDF 4.4 系)自带的
//   `esp_lcd_panel_rgb.h` 是**旧的精简版**(129 行),只有
//     · 单个 `cfg.on_frame_trans_done` 回调
//     · 一块由驱动分配的 framebuffer(`esp_lcd_panel_draw_bitmap` 往它里面 memcpy)
//   **没有** `num_fbs` / `esp_lcd_rgb_panel_get_frame_buffer()` /
//   `esp_lcd_rgb_panel_register_event_callbacks()` / bounce buffer
//   ⇒ 只能"一边扫描一边往同一块 fb 里写",**撕裂是结构性的**,再怎么调时机都是
//     "把缝挪到别处"(上一轮实测:8KB 定额 + 每块等消隐期 ⇒ 从花屏变成
//     "一条横扫的缝",整屏刷新还要 0.85 秒)。
//
// 现在这份平台是 **pioarduino espressif32 55.03.39 = arduino-esp32 3.3.9 +
// ESP-IDF 5.5.4**(只换 [env:esp32s3-rgb] 这一条 env,见 platformio.ini 那段),
// IDF 5.5 的 `esp_lcd_panel_rgb.h` 有那三样 ⇒ 换成**双缓冲 + vsync 换帧**:
//
//   · `num_fbs = 2`,`flags.fb_in_psram = 1` ⇒ 驱动在 PSRAM 里分配**两块**
//     480×480×2B = 450KB 的整屏 fb(`esp_lcd_rgb_panel_get_frame_buffer()` 取地址);
//   · 驱动只把 **cur_fb_index 那一块**交给 LCD_CAM 的 DMA 连续扫描;
//   · 我们的 flush 永远往**不在扫的那一块**(back)里画;
//   · 一次 LVGL 刷新画完之后,用 `esp_lcd_panel_draw_bitmap(panel, 0,0,W,1, back)`
//     把驱动的 cur_fb_index 指到 back —— 传的指针落在 fb 范围内时,驱动走的是
//     "draw buffer 就是帧缓冲"那一支(`esp_lcd_panel_rgb.c`:`draw_buf_copy_to_fb
//     = false`):**它不拷贝**,只改 cur_fb_index,并在 stream_mode 下把 DMA 的
//     帧缓冲链表重新串到新 fb 上 ⇒ **在下一个帧边界(消隐期)整块换过去**,
//     换帧那一刻屏幕上只有"上一幅"或"下一幅",不存在半新半旧 ⇒ **无撕裂**。
//
// ★ 为什么坐标给 (0, 0, W, 1) 而不是整屏:那一支里驱动还会对"这次窗口"做一次
//   cache 回写(`esp_cache_msync`),给整屏就是每次换帧都回写 450KB;我们自己
//   已经对**真正写过的区域**做过回写(见 blit_area),所以这里只要一行,
//   把驱动那次回写压到最小 —— 换帧因此是**纯指针操作**,几十微秒。
//
// ★★ "写 back 之前"的那道门(wait_swap_settled,看 `swap_wait`/`timeout` 两个计数):
//   换帧请求是**立刻**改 cur_fb_index 的,但 DMA 要到**下一个帧边界**才真的换过去
//   —— 也就是说,换帧后的一小段(≤1 帧 = 18MHz 下 15.5ms)里,旧的那块**还在被扫**。
//   所以下一次 flush 动手之前必须确认那一个边界已经过去,否则那一笔就会落在
//   正在扫描的块上(又是撕裂)。做法是等 on_vsync 计数越过换帧时的计数:
//   稳态下 UI 每 200ms 才画一次,这个门**从来不阻塞**(一次比较就过);
//   开机动画 50Hz 档最坏等一帧。等不到(60ms)就放行并 ++timeout —— 绝不死等。
//
// ★ 两块 fb 的"内容一致"是怎么保证的(否则换过去会看到上一轮的残影):
//   `g_fb_complete[i]` 记"这块 fb 里是不是一整幅完整画面"。往一块**还没完整**的
//   fb 上画之前,先把另一块(完整的那块)整块拷过来(450KB 一次,只在开机后第一次
//   换帧之后发生一次)⇒ 之后每块 fb 都等于"上一幅完整画面",局部刷新叠上去
//   自然就是新的完整画面(见 flush 里那段)。稳态下**不做任何整块拷贝**。
//
// 代价与边界(写清楚,别指望它包打天下):
//   · 换帧延迟 = 最坏 1 帧(18MHz 15.5ms、30MHz 9.3ms),肉眼不可见;
//   · 整屏刷新 = 450KB 的 CPU→PSRAM 拷贝(实测几十毫秒量级,见 ACCEPTANCE),
//     而且**不再需要**"按 8KB 分块 + 每块等消隐期"那套节流;
//   · 双 fb 各 450KB ⇒ 900KB PSRAM 常驻(板上有 8189KB,见自检那行)。
// ============================================================

#include "dash_display.h"
#include "ui_theme.h"
#include "dash_log.h"     // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

#if defined(DASH_DISPLAY_RGB)

#include <Arduino.h>
#include <string.h>              // memcpy(往 back fb 里搬像素)
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_rgb.h>   // ★ IDF 5.5 的版本:num_fbs / register_event_callbacks
#include <esp_lcd_panel_ops.h>
#include <esp_cache.h>           // esp_cache_msync():CPU 写过的 PSRAM 要回写给 DMA 看
#include <driver/spi_common.h>   // SPI2_HOST(初始化命令那条 3 线 SPI 用)
#include <driver/spi_master.h>   // 裸 spi_device_transmit(见下面第 4 块的说明)
#include <esp_heap_caps.h>

// ------------------------------------------------------------
// ★ 屏到手后**只改这个文件顶部的数字**,别处的代码不用动。
//   下面每一项都标了"从哪来",因为 ST7701 的初始化时序/上电顺序各家不同,
//   抄错一项就是黑屏或者花屏(而且不报错)。
//
//   ★★ 2026-09-24 这块板已经点起来了:**微雪 ESP32-S3-LCD-2.8C(非触控,最终板)**。
//   实际生效的引脚/时序/41 步初始化、出处、以及两个"静默失败"的坑,
//   都记在 **docs/RGB-PANEL-2.8C.md** 里 —— 换板/换屏之前先看那一页。
// ------------------------------------------------------------

// ---- 1) 引脚:**微雪 ESP32-S3-LCD-2.8C(非触控,最终板)** ----
//   出处(可复核):官方例程包 ESP32-S3-LCD-2.8C-Demo.zip 里
//     Arduino/examples/LVGL_Arduino/Display_ST7701.h —— 与 wiki 的
//     2.8C 引脚表逐脚一致(LCD_BL=GPIO6 / PCLK=41 / DE=40 / VSYNC=39 / HSYNC=38)。
//   ★ 这块板的**屏接口引脚表与 2.1" 那族逐脚相同**(3 份官方例程逐行比过:
//     ESP32-S3-LCD-2.8C / ESP32-S3-Touch-LCD-2.8C / ESP32-S3-Touch-LCD-2.1)
//     ⇒ 引脚这块**不需要按板子分支**;按板子分的只有初始化和时序(第 2/3 块)。
//   ★ 数据线 bit0 接的是面板 **B1**(B0=NC)、bit5=G0、bit11=**R1**(R0=NC):
//     即面板 B0/R0 两根最低位不接,其余 16 根按 RGB565 顺序连号。**照抄,别重排** ——
//     换序的症状是"颜色整体偏色/红蓝互换",而屏是亮的,很容易误判成时序问题。
#define RGB_PIN_PCLK   41
#define RGB_PIN_DE     40
#define RGB_PIN_VSYNC  39
#define RGB_PIN_HSYNC  38
#define RGB_DATA_GPIOS { 5, 45, 48, 47, 21, 14, 13, 12, \
                         11, 10, 9, 46, 3, 8, 18, 17 }

// ---- 2) 初始化命令:**照抄 2.8C 官方例程的 ST7701 上电序列**(41 步) ----
//   来源:ESP32-S3-LCD-2.8C-Demo.zip → Display_ST7701.cpp 的 ST7701_Init()。
//   ★ 触控版(Touch-2.8C)与非触控版这一段的**逐条相同**(两份例程 diff 过:
//     IDENTICAL)⇒ 这一段按**面板**走、不按板子走。
//   ★ 三类细节别自作聪明改:
//     · 开头那三行 `0xFF 77 01 00 00 13` → `0xEF 08` → `0xFF …10` 是 2.8C 的
//       **入口页顺序**:先页 0x13 把 0xEF 置 8,再回页 0x10 写电源/伽马。顺序错了
//       后面整段都落错页(屏黑,而串口一切正常)。
//     · `0xC1=0x10 0x0C` / `0xC2=0x07 0x0A` 是 VBP/VFP 那一组,**别与 2.1" 的
//       `0x0B 0x02`/`0x07 0x02` 混用** —— 那是另一块面板的数。
//     · 0x11(SLPOUT)后 **120ms**,然后 0x3A=0x66 → 0x36=0x00 → 0x35=0x00(TEON)
//       → 0x29(DISPON)。★ 2.8C **没有** 0x20(INVOFF)那一步(2.1" 才有),
//       而多一步 0x35;这两条是两块面板最容易抄串的地方。
//   走 3 线 SPI(SCLK/SDA)写命令;**CS 不在 GPIO 上**,在 TCA9554 的 EXIO3 上,
//   所以下面 io_cfg.cs_gpio_num = -1,由 tca9554_* 手动拉(见第 4 块)。
#define RGB_PIN_INIT_SDA   1
#define RGB_PIN_INIT_SCLK  2
struct RgbInitCmd { uint8_t cmd; uint8_t data[16]; uint8_t len; uint16_t delay_ms; };
static const RgbInitCmd kPanelInit[] = {
  { 0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0 },   // 入口页 0x13
  { 0xEF, {0x08}, 1, 0 },
  { 0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0 },   // 回页 0x10
  { 0xC0, {0x3B, 0x00}, 2, 0 },                     // Scan line
  { 0xC1, {0x10, 0x0C}, 2, 0 },                     // VBP(2.8C)
  { 0xC2, {0x07, 0x0A}, 2, 0 },                     // VFP(2.8C)
  { 0xC7, {0x00}, 1, 0 },
  { 0xCC, {0x10}, 1, 0 },
  { 0xCD, {0x08}, 1, 0 },                           // RGB format
  { 0xB0, {0x05, 0x12, 0x98, 0x0E, 0x0F, 0x07, 0x07, 0x09,
           0x09, 0x23, 0x05, 0x52, 0x0F, 0x67, 0x2C, 0x11}, 16, 0 },   // IPS
  { 0xB1, {0x0B, 0x11, 0x97, 0x0C, 0x12, 0x06, 0x06, 0x08,
           0x08, 0x22, 0x03, 0x51, 0x11, 0x66, 0x2B, 0x0F}, 16, 0 },   // IPS
  { 0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0 },   // 页 0x11
  { 0xB0, {0x5D}, 1, 0 },                           // VOP
  { 0xB1, {0x3E}, 1, 0 },                           // VCOM amplitude
  { 0xB2, {0x81}, 1, 0 },                           // VGH 12V
  { 0xB3, {0x80}, 1, 0 },
  { 0xB5, {0x4E}, 1, 0 },                           // VGL
  { 0xB7, {0x85}, 1, 0 },
  { 0xB8, {0x20}, 1, 0 },
  { 0xC1, {0x78}, 1, 0 },
  { 0xC2, {0x78}, 1, 0 },
  { 0xD0, {0x88}, 1, 0 },
  { 0xE0, {0x00, 0x00, 0x02}, 3, 0 },
  { 0xE1, {0x06, 0x30, 0x08, 0x30, 0x05, 0x30, 0x07,
           0x30, 0x00, 0x33, 0x33}, 11, 0 },
  { 0xE2, {0x11, 0x11, 0x33, 0x33, 0xF4, 0x00,
           0x00, 0x00, 0xF4, 0x00, 0x00, 0x00}, 12, 0 },
  { 0xE3, {0x00, 0x00, 0x11, 0x11}, 4, 0 },
  { 0xE4, {0x44, 0x44}, 2, 0 },
  { 0xE5, {0x0D, 0xF5, 0x30, 0xF0, 0x0F, 0xF7, 0x30, 0xF0,
           0x09, 0xF1, 0x30, 0xF0, 0x0B, 0xF3, 0x30, 0xF0}, 16, 0 },
  { 0xE6, {0x00, 0x00, 0x11, 0x11}, 4, 0 },
  { 0xE7, {0x44, 0x44}, 2, 0 },
  { 0xE8, {0x0C, 0xF4, 0x30, 0xF0, 0x0E, 0xF6, 0x30, 0xF0,
           0x08, 0xF0, 0x30, 0xF0, 0x0A, 0xF2, 0x30, 0xF0}, 16, 0 },
  { 0xE9, {0x36, 0x01}, 2, 0 },
  { 0xEB, {0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40}, 7, 0 },
  { 0xED, {0xFF, 0x10, 0xAF, 0x76, 0x54, 0x2B, 0xCF, 0xFF,
           0xFF, 0xFC, 0xB2, 0x45, 0x67, 0xFA, 0x01, 0xFF}, 16, 0 },
  { 0xEF, {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0 },
  { 0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0 },  // 回页 0
  { 0x11, {}, 0, 120 },                            // SLPOUT(2.8C 是 120ms)
  { 0x3A, {0x66}, 1, 0 },                          // COLMOD(见上面 ★)
  { 0x36, {0x00}, 1, 0 },                          // MADCTL:扫描方向
  { 0x35, {0x00}, 1, 0 },                          // TEON(2.8C 有,2.1" 没有)
  { 0x29, {}, 0, 0 },                              // DISPON(2.8C 无 0x20 那步)
};
static const size_t kPanelInitCount = sizeof(kPanelInit) / sizeof(kPanelInit[0]);

// ---- 3) 时序:porch **照抄 2.8C 官方例程**;PCLK 见下 ----
//   像素时钟 = (h_res + 前后沿/脉宽) × (v_res + 前后沿/脉宽) × 刷新率。
//     · 30MHz:官方 **Arduino** 例程的值(porch 与这里逐项相同);
//     · 18MHz:官方 **ESP-IDF** 例程的值(`EXAMPLE_LCD_PIXEL_CLOCK_HZ`)。
//   ★ 2026-09-24(旧栈、单 fb):30MHz 下**每次画面更新整屏花** —— 那时候没有
//     bounce buffer、CPU 和 DMA 抢同一块 PSRAM,写一下就 FIFO 欠载;降到 18MHz
//     并把写 fb 挪到消隐期之后才干净。
//   ★ 2026-09-24(本栈、双 fb):**先按 18MHz 实测,再单独试 30MHz**,以实测为准,
//     不稳就退回 18MHz —— 见 docs/RGB-PANEL-2.8C.md 第 9 节的实测表。
//   ⇒ 本机当前:**18MHz** / ((480+8+10+50) × (480+2+18+8)) = 18e6/548/508 ≈ **64.7 Hz**。
//     (串口上 `rgb: vsync=…(+N/s)` 的 N 就是这个量级 —— 它同时是"PCLK 到底跑成
//      多少"的**第一手判据**:18MHz→约 65、30MHz→约 108。)
#define RGB_PIXEL_CLOCK_HZ  (18 * 1000 * 1000)   // ← 定案：30MHz 也实测过(帧率 107.8Hz
                                                   //   对得上)，但同一帧里往 fb 里搬像素的耗时从 19.9ms 涨到 27.2ms
                                                   //   —— 那正是 PSRAM 争用的信号；本栈没开 bounce buffer，所以定 18MHz（见 docs 第 9 节）
#define RGB_HSYNC_PULSE     8                    // HPW
#define RGB_HSYNC_BACK      10                   // HBP
#define RGB_HSYNC_FRONT     50                   // HFP
#define RGB_VSYNC_PULSE     2                    // VPW(2.8C;2.1" 是 3)
#define RGB_VSYNC_BACK      18                   // VBP(2.8C;2.1" 是 8)
#define RGB_VSYNC_FRONT     8                    // VFP
// 例程里这三个都是 0:hsync_idle_low=0 / vsync_idle_low=0 / pclk_active_neg=false
// (占位版写的是 idle_low=1 —— 那是另一族的常见值,在这块屏上会让画面整行错位)
#define RGB_PCLK_ACTIVE_NEG 0
#define RGB_HSYNC_IDLE_LOW  0
#define RGB_VSYNC_IDLE_LOW  0

// 一屏的字节数(480×480×RGB565)。两块 fb 各这么大,都在 PSRAM。
#define RGB_FB_BYTES  ((uint32_t)THEME_DISPLAY_RES * (uint32_t)THEME_DISPLAY_RES * 2u)

// ------------------------------------------------------------
// 双缓冲状态
// ------------------------------------------------------------
static esp_lcd_panel_handle_t g_panel = nullptr;
static lv_display_t* g_left = nullptr;
static lv_display_t* g_right = nullptr;
static uint8_t* g_fb[2] = {nullptr, nullptr};   // 驱动分配的**两块**整屏 fb(PSRAM)

// 哪一块正在被 DMA 扫描(g_front)、我们往哪一块画(g_back)。
//   ★ 只在 flush 里改(LVGL 的刷新跑在主循环),ISR 只读不写。
static uint8_t g_front = 0;
static uint8_t g_back = 1;
// 这块 fb 里是不是"一整幅完整画面"。往一块不完整的 fb 上画之前要先从完整的
// 那块整块拷过来(见 flush 里那段),否则换过去会看到上一轮的残影/花屏。
static bool g_fb_complete[2] = {false, false};

static volatile uint32_t g_vsync = 0;        // on_vsync 回调计数(= 面板扫描帧数)
static uint32_t g_swap = 0;                  // 换帧次数(一次 LVGL 刷新 = 一次)
static uint32_t g_swap_vsync = 0;            // 最近一次换帧时的 vsync 计数
static uint32_t g_flush_count = 0;           // flush 回调次数

// 诊断:等"换帧那个边界过去"的统计(判据是 timeout 恒为 0)
static uint32_t g_swap_wait_max_us = 0;
static uint32_t g_swap_timeout = 0;
static uint32_t g_skip_count = 0;            // 面板没建起来时直接放行的次数

// 诊断:往 fb 里搬像素(含 cache 回写)的耗时
static uint32_t g_copy_us_max = 0;
static uint32_t g_copy_us_sum = 0;
static uint32_t g_copy_n = 0;
static uint32_t g_msync_n = 0;               // 顺带:整块补拷(把完整画面同步到另一块)的次数

// 本次刷新覆盖了哪些**整行**(只统计"整行都写了"的,用位图记 —— 480 行 = 15 个 u32)。
//   它的唯一用途:判断这一次刷新是不是"整屏"(是的话,目标 fb 从此算完整)。
//   ★ THEME_DISPLAY_RES=480 是 32 的整数倍,所以"全 1"就是"所有行都覆盖"。
#define RGB_ROW_WORDS  (((int)THEME_DISPLAY_RES + 31) / 32)
static uint32_t g_rows_full[RGB_ROW_WORDS];

static void rows_reset() {
  for (int i = 0; i < RGB_ROW_WORDS; ++i) g_rows_full[i] = 0u;
}
static void rows_mark(int32_t y1, int32_t y2) {
  for (int32_t y = y1; y <= y2; ++y) {
    const int w = (int)(y >> 5);
    if (w >= 0 && w < RGB_ROW_WORDS) g_rows_full[w] |= (1u << (uint32_t)(y & 31));
  }
}
static bool rows_all() {
  const uint32_t last_mask = (((uint32_t)THEME_DISPLAY_RES % 32u) != 0u)
      ? ((1u << ((uint32_t)THEME_DISPLAY_RES % 32u)) - 1u) : 0xFFFFFFFFu;
  for (int i = 0; i < RGB_ROW_WORDS; ++i) {
    const uint32_t want = (i == RGB_ROW_WORDS - 1) ? last_mask : 0xFFFFFFFFu;
    if (g_rows_full[i] != want) return false;
  }
  return true;
}

// 一次刷新的计时(给"开机整屏刷新耗时"那条日志用)
static uint32_t g_refr_t0_us = 0;
static uint32_t g_refr_copy_us = 0;
static uint32_t g_refr_flush_n = 0;

// ------------------------------------------------------------
// on_vsync:**每帧一次的中断**(IDF 5.5 的 RGB 面板回调表里的一项)
//
//   ★ 它是这一版**唯一的**扫描同步信号,只做一件事:计数。
//   · 计数 g_vsync 就是"面板确实在收帧"的判据(和旧栈的 `frames=` 同一个用途),
//     同时也是"上一次换帧那个边界过去了没有"的判据(见 wait_swap_settled);
//   · **绝不在中断里碰显存**(那是 flush 的事),也不能在这里调任何阻塞函数。
//   ★ 这个回调在**中断上下文**里跑,所以标 IRAM_ATTR(IDF 在
//     CONFIG_LCD_RGB_ISR_IRAM_SAFE 下会直接拒收不在 IRAM 里的回调)。
// ------------------------------------------------------------
static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                               const esp_lcd_rgb_panel_event_data_t* edata,
                               void* user_ctx) {
  (void)panel; (void)edata; (void)user_ctx;
  // 不用 `++g_vsync`：C++20 起对 volatile 的自增/复合赋值已弃用（GCC 报 -Wvolatile），
  // 写开是同一件事，而且只有一个写者（这个 ISR）。
  g_vsync = g_vsync + 1u;
  return false;   // 没唤醒高优先级任务
}

// ------------------------------------------------------------
// 换帧的那道门:等"上一次换帧的那个帧边界"过去
//
//   为什么要等:换帧请求(draw_bitmap 指到 back)**立刻**改了驱动的 cur_fb_index,
//   但 DMA 要到**下一个帧边界**才真的从新 fb 取像素 —— 在那之前旧的那块还在被扫。
//   这时候如果往"新的 back(= 刚被换下去的那块)"里写,那一笔就落在正在扫描的
//   显存上 ⇒ 又是一条缝。所以动手前先确认 on_vsync 已经越过换帧时的计数。
//
//   ★ 稳态(UI 每 200ms 画一次)下这个门**一次都不阻塞**:一次比较就过。
//   ★ 兜底:60ms 还没等到(面板被停/中断没来)就放行并 ++timeout,**绝不死等**
//     —— 死在这里的后果是整屏再也不更新(上一轮踩过)。
// ------------------------------------------------------------
static void wait_swap_settled() {
  if (g_vsync != g_swap_vsync) return;      // 边界已经过去(绝大多数情况走这一行)
  const uint32_t t0 = micros();
  while (g_vsync == g_swap_vsync) {
    if ((uint32_t)(micros() - t0) >= 60000u) { ++g_swap_timeout; return; }
  }
  const uint32_t waited = (uint32_t)(micros() - t0);
  if (waited > g_swap_wait_max_us) g_swap_wait_max_us = waited;
}

// 把一块区域从 LVGL 的绘制缓冲搬进某块 fb,并把这一段回写进 PSRAM。
//   · LVGL v9 的 px 指向绘制缓冲,区域内容按**区域自己的行距**紧排,而 fb 的行距
//     是整屏宽(960B)⇒ 区域不是整宽时按行拷;
//   · ★ cache 回写**不能省**:fb 在 PSRAM、在 cache 后面(驱动自己也这么干:
//     `esp_lcd_panel_rgb.c` 里拷完就 `esp_cache_msync`),不回写的话 DMA
//     读到的还是旧内容(症状是"画面里混着上一帧的碎片")。
static void blit_area(uint8_t* dst_fb, const uint8_t* src,
                      int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
  const uint32_t row_bytes = (uint32_t)(x2 - x1 + 1) * 2u;      // 区域一行多少字节
  const uint32_t fb_stride = (uint32_t)THEME_DISPLAY_RES * 2u;  // fb 一行多少字节
  const uint32_t off = (uint32_t)y1 * fb_stride + (uint32_t)x1 * 2u;
  const uint32_t nbytes = (uint32_t)(y2 - y1 + 1) * row_bytes;
  if (nbytes == 0u) return;
  uint8_t* dst = dst_fb + off;
  if (row_bytes == fb_stride) {
    memcpy(dst, src, nbytes);                 // 整宽区域:一次拷完
  } else {
    for (int32_t y = y1; y <= y2; ++y) {      // 窄区域:按行拷
      memcpy(dst, src, row_bytes);
      dst += fb_stride;
      src += row_bytes;
    }
  }
  esp_cache_msync((void*)(dst_fb + off), nbytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// 整块补拷:把"完整的那块 fb"整幅搬到目标 fb(开机后第一次换帧之后发生一次)。
static void sync_full_fb(uint8_t dst_idx, uint8_t src_idx) {
  const uint32_t t0 = micros();
  memcpy(g_fb[dst_idx], g_fb[src_idx], RGB_FB_BYTES);
  esp_cache_msync((void*)g_fb[dst_idx], RGB_FB_BYTES,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  g_copy_us_sum += (uint32_t)(micros() - t0);
  ++g_copy_n;
  ++g_msync_n;
}

// 换帧:把驱动的 cur_fb_index 指到 back —— DMA 会在**下一个帧边界**整块换过去。
//
//   ★ 传的指针落在 fb 范围内 ⇒ 驱动走 `draw_buf_copy_to_fb = false` 那一支:
//     **不拷贝**,只改 cur_fb_index + 在 stream_mode 下重串 DMA 的帧缓冲链表。
//   ★ 窗口给 (0,0,W,1):那一支里驱动会对"这次窗口"做一次 cache 回写,给整屏
//     就是每次换帧回写 450KB —— 我们自己已经回写过真正写过的区域了,所以这里
//     只要一行,让驱动那次回写退化成 960B。换帧本身因此是**几十微秒**的事。
static void request_swap() {
  if (g_panel == nullptr || g_fb[0] == nullptr) return;

  // 这一幅画完之后,目标 fb 算不算"完整的一幅画面"?
  const bool full = rows_all();
  if (full) g_fb_complete[g_back] = true;

  const uint32_t t0 = micros();
  esp_lcd_panel_draw_bitmap(g_panel, 0, 0, (int)THEME_DISPLAY_RES, 1, g_fb[g_back]);
  const uint32_t dt = (uint32_t)(micros() - t0);

  g_front = g_back;
  g_back = (uint8_t)(1u - g_front);
  g_swap_vsync = g_vsync;      // 从现在起,"下一个 vsync"就是换帧生效的那个边界
  ++g_swap;
  if (dt > g_copy_us_max) g_copy_us_max = dt;   // 换帧本身也记进最大值(它极小)

  if (full) {
    // 整屏刷新:把"从第一块 flush 到换帧"这段耗时打出来 —— 这就是
    // "开机整屏刷新耗时"那个数(见 ACCEPTANCE / docs 第 9 节)。
    dash_logf("rgb: 整屏刷新 %.1fms(块=%u 拷贝%.1fms 数据%.0fKB) 换帧在下一个vsync\n",
                  (double)(micros() - g_refr_t0_us) / 1000.0,
                  (unsigned)g_refr_flush_n, (double)g_refr_copy_us / 1000.0,
                  (double)RGB_FB_BYTES / 1024.0);
  }
  rows_reset();
  g_refr_copy_us = 0;
  g_refr_flush_n = 0;
}

// LVGL → 面板:把这一块搬进 **back** fb;一次刷新的最后一块再请求换帧。
static void rgb_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  ++g_flush_count;
  if (g_panel == nullptr || g_fb[0] == nullptr) {   // 没面板:直接放行,别把 LVGL 卡死
    ++g_skip_count;
    lv_display_flush_ready(disp);
    return;
  }
  if (g_refr_flush_n == 0) g_refr_t0_us = micros();   // 本次刷新的第一块:起表
  ++g_refr_flush_n;

  // ① 裁剪(LVGL 不该给越界的区域,这里只是不信任输入)
  int32_t x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > (int32_t)THEME_DISPLAY_RES - 1) x2 = (int32_t)THEME_DISPLAY_RES - 1;
  if (y2 > (int32_t)THEME_DISPLAY_RES - 1) y2 = (int32_t)THEME_DISPLAY_RES - 1;
  if (x2 < x1 || y2 < y1) { lv_display_flush_ready(disp); return; }

  // ② 动手之前:确认上一次换帧的那个边界已经过去(见 wait_swap_settled)
  wait_swap_settled();

  // ③ 目标 fb 还不完整 ⇒ 先把完整的那块整幅搬过来(开机后只发生一次)
  if (!g_fb_complete[g_back] && g_fb_complete[g_front]) {
    sync_full_fb(g_back, g_front);
    g_fb_complete[g_back] = true;
  }

  // ④ 搬像素(只往 back 写 ⇒ 不碰正在扫描的那块 ⇒ 无撕裂)
  const uint32_t c0 = micros();
  blit_area(g_fb[g_back], px, x1, y1, x2, y2);
  const uint32_t dt = (uint32_t)(micros() - c0);
  g_copy_us_sum += dt;
  g_refr_copy_us += dt;
  ++g_copy_n;
  if (dt > g_copy_us_max) g_copy_us_max = dt;

  // ⑤ 记"这一行是整行写的"(整屏判据用)
  if (x1 == 0 && x2 == (int32_t)THEME_DISPLAY_RES - 1) rows_mark(y1, y2);

  // ⑥ 一次刷新的最后一块:请求换帧(vsync 边界整块换过去)
  if (lv_display_flush_is_last(disp)) request_swap();

  lv_display_flush_ready(disp);
}

// ------------------------------------------------------------
// 4) 板载 TCA9554PWR(I2C 扩展):**RESET 与 CS 都不在 GPIO 上**
//
//   这块板把 LCD_RST 放在 EXIO1、LCD_CS 放在 EXIO3(wiki 引脚表的 "EXIO1/EXIO3"
//   就是这里),所以"点屏"除了 SPI/RGB 那二十来根线,还必须先打通 I2C:
//       · I2C:SCL=GPIO7 / SDA=GPIO15(12PIN 上那两根,板上共用;见 wiki 接口表)
//       · TCA9554 地址 0x20;寄存器 0x01=输出、0x03=方向(0=输出)
//       · EXIO 编号按**位**算:EXIO1=bit0、EXIO3=bit2、EXIO8=bit7(蜂鸣器)
//   ★ 顺序是硬的:`Wire.begin` → 方向全设输出 → 拉 RST 低→高 → **再**拉 CS 低,
//     然后才开始写初始化命令。少任何一步的症状都是"屏全黑,而串口一切正常"。
//   ★ 例程还把 EXIO8 拉低(蜂鸣器关)。这里也拉一下:输出寄存器复位值本来就是 0,
//     显式写一次是为了"以后谁想在 EXIO 上加点什么"时不会先被蜂鸣器吓一跳。
// ------------------------------------------------------------
#include <Wire.h>
#define TCA9554_ADDR        0x20
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_CONFIG  0x03
#define LCD_RST_EXIO_BIT    0    // EXIO1
#define LCD_CS_EXIO_BIT     2    // EXIO3
#define BUZZER_EXIO_BIT     7    // EXIO8
#define RGB_PIN_BL          6    // LCD_BL:背光(高有效,板上经 MOS 管)

static uint8_t g_exio_out = 0x00;   // 影子寄存器:Set_EXIO 是"读-改-写",别丢别的位

static void tca9554_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void tca9554_begin() {
  Wire.begin(15, 7);                  // SDA=GPIO15, SCL=GPIO7
  tca9554_write(TCA9554_REG_CONFIG, 0x00);   // 8 个口全设成输出(例程 TCA9554PWR_Init(0x00))
  tca9554_write(TCA9554_REG_OUTPUT, g_exio_out);
}

static void tca9554_set(uint8_t bit, bool high) {
  if (high) g_exio_out |= (uint8_t)(1u << bit);
  else      g_exio_out &= (uint8_t)~(1u << bit);
  tca9554_write(TCA9554_REG_OUTPUT, g_exio_out);
}

// 背光:PWM 走 LEDC。★ 这块框架是 arduino-esp32 **3.3.9**,LEDC 的 API 在 3.x
// 换过一次:2.x 是 `ledcSetup(通道,频率,位数)` + `ledcAttachPin(脚,通道)` 两步,
// 3.x 合成一步 **`ledcAttach(脚, 频率, 位数)`**,之后 `ledcWrite(脚, 占空比)`
// (按**脚**寻址,不再是我们自己挑通道)。★ 频率/位数/占空比一个字没变:
// 例程用 20kHz / 10 位 / 50%,这里照抄(频率落在人耳外,不会听见啸叫)。
#define RGB_BL_LEDC_HZ   20000
#define RGB_BL_LEDC_BITS 10
#define RGB_BL_DUTY      512    // 10 位的一半 ≈ 50%

// 写一条初始化命令:**裸 spi_master**(和官方例程 Display_ST7701.cpp 一模一样)
//
// ★ 为什么不用 esp_lcd_panel_io_spi(骨架原稿的写法):
//   ST7701 的 3 线 SPI 是"**9 位**"帧 —— 第 1 位 0=命令、1=数据,后面 8 位是内容,
//   例程把它表达成 spi_device 的 `command_bits=1 + address_bits=8`(这是 esp_lcd
//   那套 IO 里没有的形状;而且 CS 还不在 GPIO 上,-1 之后总线谁初始化的也不确定)。
//   例程那段是**在这块板上验过**的,直接照抄最省事:总线自己 init、设备自己 add。
static spi_device_handle_t g_spi = nullptr;

static void st7701_tx(uint8_t is_data, uint8_t v) {
  if (g_spi == nullptr) return;
  spi_transaction_t t = {};
  t.cmd = is_data ? 1 : 0;    // command_bits=1:0=命令,1=数据
  t.addr = v;                 // address_bits=8:内容
  t.length = 0;               // 没有数据阶段
  spi_device_transmit(g_spi, &t);
}

static void panel_init_sequence() {
  if (g_spi == nullptr) return;
  for (size_t i = 0; i < kPanelInitCount; ++i) {
    const RgbInitCmd& c = kPanelInit[i];
    st7701_tx(0, c.cmd);
    for (uint8_t k = 0; k < c.len; ++k) st7701_tx(1, c.data[k]);
    if (c.delay_ms) delay(c.delay_ms);
  }
}

void dash_display_init() {
  // ---- ① 先把 RST/CS 那两颗扩展口的片子叫醒(TCA9554)----
  tca9554_begin();
  tca9554_set(BUZZER_EXIO_BIT, false);
  // 复位脉冲:低 10ms → 高 → 等 50ms(例程 ST7701_Reset() 的时序)
  tca9554_set(LCD_RST_EXIO_BIT, false); delay(10);
  tca9554_set(LCD_RST_EXIO_BIT, true);  delay(50);

  // ---- ② 3 线 SPI:只用来写初始化命令,不进画 ----
  //   SCLK=GPIO2 / MOSI(SDA)=GPIO1,SPI2_HOST、模式 0、40MHz(例程的取值)。
  //   ★ CS 由 EXIO3 手动拉(下面 tca9554_set),所以这里 spics_io_num = -1:
  //     填成某个 GPIO 的话,驱动会去动一根**没接屏**的脚,而真正的 CS 一直浮着
  //     —— 症状同样是全黑。
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = RGB_PIN_INIT_SDA;
  buscfg.miso_io_num = -1;
  buscfg.sclk_io_num = RGB_PIN_INIT_SCLK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 64;
  esp_err_t berr = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (berr != ESP_OK) {
    dash_logf("rgb: 3线SPI 总线初始化失败 err=%d\n", (int)berr);
  }
  spi_device_interface_config_t devcfg = {};
  devcfg.command_bits = 1;      // ← 第 9 位:0=命令 / 1=数据
  devcfg.address_bits = 8;      // ← 后 8 位:内容
  devcfg.mode = 0;
  devcfg.clock_speed_hz = 40 * 1000 * 1000;
  devcfg.spics_io_num = -1;     // CS 在 EXIO3 上(见上)
  devcfg.queue_size = 1;
  esp_err_t derr = spi_bus_add_device(SPI2_HOST, &devcfg, &g_spi);
  if (derr != ESP_OK) {
    dash_logf("rgb: 3线SPI 设备注册失败 err=%d\n", (int)derr);
  }
  tca9554_set(LCD_CS_EXIO_BIT, false);   // CS 低:开始收命令
  delay(10);
  panel_init_sequence();
  tca9554_set(LCD_CS_EXIO_BIT, true);    // CS 高:命令写完就不再用 SPI 了
  delay(10);

  // ---- RGB 并口 ----
  esp_lcd_rgb_panel_config_t cfg = {};
  // ★ 18MHz 走 PLL160M(160/18≈8.89,分频器带小数部分,能凑准)。
  //   换了 pclk 记得一起看这一行:30MHz 也在这个源上试过(见文件头第 3 块)。
  cfg.clk_src = LCD_CLK_SRC_PLL160M;
  cfg.timings.pclk_hz = RGB_PIXEL_CLOCK_HZ;
  cfg.timings.h_res = THEME_DISPLAY_RES;
  cfg.timings.v_res = THEME_DISPLAY_RES;
  cfg.timings.hsync_pulse_width = RGB_HSYNC_PULSE;
  cfg.timings.hsync_back_porch = RGB_HSYNC_BACK;
  cfg.timings.hsync_front_porch = RGB_HSYNC_FRONT;
  cfg.timings.vsync_pulse_width = RGB_VSYNC_PULSE;
  cfg.timings.vsync_back_porch = RGB_VSYNC_BACK;
  cfg.timings.vsync_front_porch = RGB_VSYNC_FRONT;
  cfg.timings.flags.pclk_active_neg = RGB_PCLK_ACTIVE_NEG;
  cfg.timings.flags.hsync_idle_low = RGB_HSYNC_IDLE_LOW;
  cfg.timings.flags.vsync_idle_low = RGB_VSYNC_IDLE_LOW;
  cfg.data_width = 16;                    // RGB565
  cfg.bits_per_pixel = 16;
  // ★★ 就是这两行把撕裂根治掉的(旧栈里这两个字段**根本不存在**,见文件头):
  cfg.num_fbs = 2;                        // 两块整屏 fb(各 450KB,在 PSRAM)
  cfg.flags.fb_in_psram = 1;
  // ★ IDF 5.5 里 `psram_trans_align`/`sram_trans_align` 已经 deprecated(同一个
  //   union 的 `dma_burst_size`);不写就是驱动默认值,别再去写那两个旧名字。
  cfg.hsync_gpio_num = RGB_PIN_HSYNC;
  cfg.vsync_gpio_num = RGB_PIN_VSYNC;
  cfg.de_gpio_num = RGB_PIN_DE;
  cfg.pclk_gpio_num = RGB_PIN_PCLK;
  cfg.disp_gpio_num = -1;                 // 背光/显示使能另接 MOSFET(见 PINOUT)
  const int data_pins[16] = RGB_DATA_GPIOS;
  for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = data_pins[i];

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &g_panel);
  if (err != ESP_OK || g_panel == nullptr) {
    // 不静默:黑屏时这一行是唯一线索
    dash_logf("rgb: 面板创建失败 err=%d(检查引脚/时序/PSRAM)\n", (int)err);
    g_panel = nullptr;
    return;
  }

  // ★ on_vsync 必须在 `esp_lcd_panel_init()` **之前**注册:`init` 里就开 DMA/开扫描,
  //   注册晚了几帧也无所谓,但"先注册、后起扫"顺序更干净。
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = on_vsync;                // 每帧一次(诊断 + 换帧边界的判据)
  err = esp_lcd_rgb_panel_register_event_callbacks(g_panel, &cbs, nullptr);
  if (err != ESP_OK) {
    dash_logf("rgb: on_vsync 回调注册失败 err=%d(换帧的边界判据就没有了)\n", (int)err);
  }

  // ★★★ 这两行**不能省**:`esp_lcd_new_rgb_panel()` 只是把面板对象建起来,
  //   真正**开 DMA / 启动 LCD_CAM 连续扫描**的是 `esp_lcd_panel_init()`。
  //   少了它,面板一个像素都不发 —— 而症状极具误导性:
  //     · 串口一切正常、"rgb: 已就绪"照打、`draw_bitmap()` 也照抄进 fb;
  //     · 屏是**纯黑**;
  //     · 唯一能看出来的数字是每秒那行 `rgb: vsync=0(+0/s)`
  //       (vsync 由 VSYNC 中断里的回调累加,没扫描就永远是 0)。
  //   2026-09-24 第一次烧上 2.1 板时踩的正是这一条(骨架原稿漏了这两行,
  //   它是照 IDF 5.x 的习惯写的;官方例程 Display_ST7701.cpp 结尾有这两句)。
  //   `esp_lcd_panel_reset()` 对 RGB 面板是空操作(没有独立复位脚,
  //   复位走的是 EXIO1,前面已经拉过了),留着是为了与例程/IDF 文档一致。
  esp_lcd_panel_reset(g_panel);
  esp_lcd_panel_init(g_panel);

  // ---- 拿到**两块** framebuffer 的地址(旧栈没有这个 API,见文件头)----
  uint32_t fb_count = 0;
  err = esp_lcd_rgb_panel_get_frame_buffer(g_panel, 2, (void**)&g_fb[0], (void**)&g_fb[1]);
  if (err != ESP_OK || g_fb[0] == nullptr || g_fb[1] == nullptr) {
    dash_logf("rgb: 取 framebuffer 失败 err=%d(拿不到双缓冲就没法无撕裂)\n", (int)err);
    g_fb[0] = g_fb[1] = nullptr;
  } else {
    fb_count = 2;
    // 驱动用 `heap_caps_aligned_calloc` 分配 ⇒ 两块都是**全 0(黑)**;
    // "完整画面"这个标记因此从 false 起(第一次整屏刷新才会把它置 true)。
    g_front = 0;
    g_back = 1;
    g_fb_complete[0] = g_fb_complete[1] = false;
    rows_reset();
  }

  // ---- 两个 lv_display:单屏版本先都画到同一块屏上 ----
  //   ★ 第二块屏到货后,把 g_right 换成它自己的 flush(见文件头"双屏出路");
  //     现在这样至少能把"两块屏各自要显示什么"的 UI 逻辑先跑通。
  lv_display_t* d0 = lv_display_create(THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  lv_display_set_flush_cb(d0, rgb_flush_cb);
  // LVGL 的绘制缓冲:**内部 SRAM 的小 PARTIAL 缓冲**(480 行里的一小段),
  // 不是整屏缓冲 —— 渲染完一段就由 flush 搬进 back fb。整屏缓冲没必要:
  // 450KB×2 已经在 PSRAM 里当 framebuffer 了(见 cfg.num_fbs)。
  // ★★ `aligned(LV_DRAW_BUF_ALIGN)` **一个字都不能少**(2026-09-24 实机踩的坑):
  //   LVGL 9.3 的 `lv_display_set_buffers()` 会先校验
  //       buf1 == lv_draw_buf_align(buf1)      // 即 buf1 必须按 LV_DRAW_BUF_ALIGN(=4) 对齐
  //   不满足就**静默 return**(LV_USE_LOG=0 时连一行警告都没有)⇒ 这条缓冲**根本没装上**。
  //   症状:面板在扫(`vsync` 每秒 +64.7,分毫不差)、UI 树齐全,但 `flush=0`、屏全黑。
  //   而 `lv_color_t` 是 24 位(3 字节)⇒ 这个数组只保证 2 字节对齐,实测 misalign=1。
  static lv_color_t draw_buf[THEME_DISPLAY_RES * 40]
      __attribute__((aligned(LV_DRAW_BUF_ALIGN)));
  lv_display_set_buffers(d0, draw_buf, nullptr, sizeof(draw_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  g_left = d0;
  g_right = d0;

  // ---- ③ 背光最后开 ----
  //   放在这里而不是开头,是为了"先有画面、再点亮":反过来的话,初始化那 600ms
  //   里屏是亮的但没内容(白/雪花),看起来像花屏,容易误判。
  ledcAttach((uint8_t)RGB_PIN_BL, (uint32_t)RGB_BL_LEDC_HZ, (uint8_t)RGB_BL_LEDC_BITS);
  ledcWrite((uint8_t)RGB_PIN_BL, (uint32_t)RGB_BL_DUTY);

  dash_logf("rgb: RGB565 %dx%d pclk=%uHz 数据位=%d 已就绪(第二块屏待接)\n",
                (int)THEME_DISPLAY_RES, (int)THEME_DISPLAY_RES,
                (unsigned)RGB_PIXEL_CLOCK_HZ, 16);
  dash_logf("rgb: 双framebuffer num_fbs=%u fb0=%p fb1=%p(各 %uKB PSRAM) "
            "on_vsync=已注册 ⇒ vsync 边界换帧(无撕裂)\n",
                (unsigned)fb_count, (void*)g_fb[0], (void*)g_fb[1],
                (unsigned)(RGB_FB_BYTES / 1024u));
  dash_logf("rgb: 板=微雪 ESP32-S3-LCD-2.8C(非触控) ST7701 RST=EXIO1 CS=EXIO3 "
            "BL=GPIO%d/PWM%d @%u%%\n",
                (int)RGB_PIN_BL, (int)RGB_BL_LEDC_HZ,
                (unsigned)(RGB_BL_DUTY * 100u / (1u << RGB_BL_LEDC_BITS)));

  // 诊断：双 fb 拿到手之后再报一次空闲内存 —— framebuffer 是 2×450KB，
  // 这一步才看得出“双缓冲到底吃掉多少 PSRAM”（自检那行跑在显示初始化之前）。
  dash_logf("rgb: 双fb 之后 空闲 PSRAM=%uKB heap=%uKB(内部)\n",
                (unsigned)(ESP.getFreePsram() / 1024u),
                (unsigned)(ESP.getFreeHeap() / 1024u));
}

lv_display_t* dash_display_left() { return g_left; }
lv_display_t* dash_display_right() { return g_right; }

// 每秒报一次帧率 —— 实屏调试时这是判断"面板到底在不在收帧"的第一手信息
// (和 VAN 那条 edges/frames 的诊断是同一个思路),同时也是**换帧健不健康**的判据:
//   · `vsync` 每秒 +N:N≈65 ⇒ PCLK 真跑在 18MHz(N≈108 ⇒ 30MHz);
//   · `swap` 跟着 flush 涨:每次 LVGL 刷新都在帧边界换了一次;
//   · **`timeout=0`**:等"换帧那个边界过去"从来没有靠超时放行;
//   · `copy_max/copy_avg`:一次往 back fb 搬像素(含 cache 回写)要多久;
//   · `fb=front/back`:当前哪块在扫、往哪块画。
void dash_display_poll() {
  static uint32_t last_ms = 0;
  static uint32_t last_vsync = 0;
  static uint32_t last_swap = 0;
  static uint32_t last_copy_sum = 0;
  static uint32_t last_copy_n = 0;
  const uint32_t now = millis();
  if (now - last_ms < 1000) return;
  const uint32_t v = g_vsync;
  const uint32_t sw = g_swap;
  const uint32_t dn = g_copy_n - last_copy_n;
  const uint32_t dsum = g_copy_us_sum - last_copy_sum;
  dash_logf("rgb: vsync=%u(+%u/s) swap=%u(+%u/s) flush=%u copy_max=%uus "
            "copy_avg=%uus swap_wait_max=%uus timeout=%u fb=%u/%u msync=%u\n",
                (unsigned)v, (unsigned)(v - last_vsync),
                (unsigned)sw, (unsigned)(sw - last_swap),
                (unsigned)g_flush_count,
                (unsigned)g_copy_us_max, (unsigned)(dn ? (dsum / dn) : 0u),
                (unsigned)g_swap_wait_max_us,
                (unsigned)g_swap_timeout,
                (unsigned)g_front, (unsigned)g_back,
                (unsigned)g_msync_n);
  last_vsync = v;
  last_swap = sw;
  last_copy_sum = g_copy_us_sum;
  last_copy_n = g_copy_n;
  last_ms = now;
}

#endif  // DASH_DISPLAY_RGB

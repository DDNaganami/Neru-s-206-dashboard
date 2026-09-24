// ============================================================
// 真实 RGB 并口屏驱动骨架(480×480,ST7701S 那类)
//
// 编译开关:`-DDASH_DISPLAY_RGB=1`(见 platformio.ini 的 [env:esp32s3-rgb])。
// 与桩驱动/预览驱动共用同一个接口(dash_display.h 的三个函数),所以
// dash_ui.cpp 一行都不用改 —— 这正是当初把它抽成接口的目的。
//
// ------------------------------------------------------------
// ★★ 先读这一段:这一版框架的 esp_lcd **是旧的**,双屏方案被它卡住
//
// 本项目用的 Espressif32 平台自带的 esp_lcd_panel_rgb.h(全部 4 个芯片目录
// 都是同一份,120 行)只有:
//     esp_lcd_new_rgb_panel(cfg, &panel)
//     cfg.on_frame_trans_done      ← 每帧转移完成回调(单个,不是回调表)
//     cfg.flags.fb_in_psram        ← 帧缓冲放 PSRAM
//     esp_lcd_panel_draw_bitmap()  ← 把内容 **memcpy 进面板自己的那块 fb**
// **没有** num_fbs / esp_lcd_rgb_panel_get_frame_buffer() /
// esp_lcd_rgb_panel_register_event_callbacks() / bounce buffer ——
// 这些是 ESP-IDF 5.x 才有的。
//
// 为什么这条决定了双屏能不能做:
//   两块 480×480 屏要共用一条 16 位数据总线(S3 没有 40 根脚给两条独立总线),
//   只能**轮流发帧**:这一帧给左屏、下一帧给右屏,选通脚跟着翻。
//   而"轮流"要求 DMA 能在**两块帧缓冲之间自动切换** —— 也就是 num_fbs=2 +
//   拿到两块 fb 的地址 + 帧切换回调。这一版框架**三样都没有** ✗
//   面板自己那块 fb 同一时刻只装得下一块屏的画面,硬做就会出现
//   "两块屏轮流显示对方的画面"(每帧 16ms 的鬼影)。
//
// ⇒ 所以本文件现在实现的是 **单屏**版本(下面),它能直接跑通一块 480×480;
//   双屏有两条出路,二选一(都不需要推翻现有代码):
//     ① **换构建**:用带 ESP-IDF 5.x esp_lcd 的构建(pioarduino 新版平台 /
//        直接用 ESP-IDF 工程)→ 拿到 num_fbs/get_frame_buffer →
//        一块 RGB 总线 + 硬件 1→2 选通(16 位缓冲 + /OE 选通脚)就能成立,
//        每块屏 30Hz。**推荐**:C 侧逻辑(本文件的时序、初始化、LVGL 绑定)都能留。
//     ② **不共用总线**:两块屏各占一组数据线 → 需要 ~40 根 ✗ S3 没有。
//
// ------------------------------------------------------------
// 单屏版本怎么工作(与本项目其余部分一致)
//   · LVGL 用**小 PARTIAL 缓冲**(SRAM 里,不占 PSRAM)渲染;
//   · flush_cb 里按区域调 esp_lcd_panel_draw_bitmap() —— 只搬变化的区域;
//   · 第二块屏先用桩(或同一块屏映射成两个 lv_display,见文件末尾的说明)。
// ============================================================

#include "dash_display.h"
#include "ui_theme.h"
#include "dash_log.h"     // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

#if defined(DASH_DISPLAY_RGB)

#include <Arduino.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>
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

// ---- 3) 时序:porch **照抄 2.8C 官方例程**;PCLK 取官方 **ESP-IDF** 例程的 18MHz ----
//   像素时钟 = (h_res + 前后沿/脉宽) × (v_res + 前后沿/脉宽) × 刷新率。
//     · 30MHz:官方 **Arduino** 例程的值(它的 porch 与这里逐项相同),
//       但那份额例程**开着 bounce buffer**(`bounce_buffer_size_px = 10*480`)。
//     · 18MHz:官方 **ESP-IDF** 例程的值(`EXAMPLE_LCD_PIXEL_CLOCK_HZ`),
//       那份例程的 bounce buffer 是**可选项** —— 我们这份旧 esp_lcd **根本没有**
//       bounce buffer(头文件里没有这个字段,反汇编也确认没有),
//       ⇒ 取 18MHz 这一档才是"不依赖 bounce buffer"的那个数。
//   ⇒ 本机:18MHz / ((480+8+10+50) × (480+2+18+8)) = 18e6/548/508 ≈ **64.7 Hz**。
//   ★ 2026-09-24 实机:30MHz 时**每次画面更新整屏花**(DMA 与 CPU 抢 PSRAM,
//     FIFO 欠载)—— 详见第 5 块那一段;降到 18MHz 并把写 fb 挪到消隐期之后才干净。
#define RGB_PIXEL_CLOCK_HZ  (18 * 1000 * 1000)
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

// ------------------------------------------------------------
// 单屏实例
// ------------------------------------------------------------
static esp_lcd_panel_handle_t g_panel = nullptr;
static lv_display_t* g_left = nullptr;
static lv_display_t* g_right = nullptr;
static volatile uint32_t g_frames = 0;      // 每帧回调里 +1(诊断用)
static volatile uint32_t g_flush_count = 0;

// LVGL 的绘制缓冲:**放 SRAM**(不是 PSRAM)—— 480×480 全屏缓冲要 450KB,
// 两块就 900KB;而"部分刷新 + 区域搬进面板 fb"这条路上,LVGL 只需要一条窄缓冲。
// 一屏 480 像素宽 × 40 行 = 38KB,够 LVGL 分批渲染。
//
// ★★ `aligned(LV_DRAW_BUF_ALIGN)` **一个字都不能少**(2026-09-24 实机踩的坑):
//   LVGL 9.3 的 `lv_display_set_buffers()` 会先校验
//       buf1 == lv_draw_buf_align(buf1)      // 即 buf1 必须按 LV_DRAW_BUF_ALIGN(=4) 对齐
//   不满足就**静默 return**(LV_USE_LOG=0 时连一行警告都没有)⇒ 这条缓冲**根本没装上**。
//   症状极具误导性,四个"看起来都对"的现象同时成立:
//     · 面板在正常扫描(`rgb: frames` 每秒 +58.5,和理论值分毫不差);
//     · UI 树齐全(屏幕 12 个子对象、两条弧都在)、LVGL 堆还剩 26KB;
//     · 主循环、tick、`lv_timer_handler()` 全都照跑;
//     · 但 **`rgb: flush=0` 永远不涨**、屏全黑。
//   而 `lv_color_t` 是 uint16_t ⇒ 这个数组**只保证 2 字节对齐**,实测 `misalign=1`。
//   (同一类坑在桩驱动 `src/dash_display.cpp` 的 `buf_left/buf_right` 上也成立 ——
//    那份**本轮没动**:它是 esp32dev / 抓帧盒在用的构建,见回报里的"没做的项"。)
static lv_color_t g_draw_buf[THEME_DISPLAY_RES * 40]
    __attribute__((aligned(LV_DRAW_BUF_ALIGN)));

// 每帧转移完成:★ 这个回调在**中断上下文**里,只允许"记个数 / 翻个脚"这种动作,
// 绝不能在这里 memcpy(那正是 draw_bitmap 干的事,必须留给主循环)。
//
// ★ 除了计数,它现在还是**唯一的扫描同步信号**(见下面 rgb_flush_cb 里那一大段):
//   这份精简 esp_lcd **没有** vsync 回调、**没有** bounce buffer、**没有** num_fbs
//   (头文件里只有 disp_active_low / relax_on_idle / fb_in_psram 三个 flag),
//   所以"帧结束"这一个中断就是我们把像素写进 fb 之前能等的**唯一**时机。
static volatile bool g_frame_done = false;   // 帧结束中断置位,主循环消费

static bool IRAM_ATTR on_frame_trans_done(esp_lcd_panel_handle_t panel,
                                         esp_lcd_rgb_panel_event_data_t* edata,
                                         void* user_ctx) {
  (void)panel; (void)edata; (void)user_ctx;
  ++g_frames;
  g_frame_done = true;
  return false;   // 没唤醒高优先级任务
}

// ------------------------------------------------------------
// 5) **扫描同步**:把"写面板 fb"这一动作推迟到帧结束(≈ 消隐期)之后
//
//   ★ 为什么非要有这一段(2026-09-24 实机症状:静态画面正常,**每次画面更新整屏都花**):
//     这一版驱动**只有一块 framebuffer**,而且它在 **PSRAM**(`fb_in_psram=1`,
//     480×480×2B=450KB 塞不进内部 SRAM)。旧 API **没有** bounce buffer
//     —— 而 bounce buffer 恰恰就是为这件事存在的:让 DMA 从**内部 SRAM** 取像素,
//     把"CPU 写 PSRAM"和"扫描读 PSRAM"彻底解耦。
//     没有它 ⇒ LCD_CAM 的 DMA 在**整帧有效像素期间**直接从 PSRAM 读(30MHz×2B
//     = 60MB/s 持续),而 LVGL 一 flush 就由 CPU 往**同一块 PSRAM**里 memcpy
//     几十 KB(`rgb_panel_draw_bitmap` 里那次 memcpy,**还会触发 cache 回写**,
//     见反汇编:`rgb_panel_draw_bitmap` 里有一处 `Cache_WriteBack_Addr`)
//     ⇒ 两边抢同一个 PSRAM 带宽,DMA FIFO 欠载(underrun)⇒ **那一帧整屏花**,
//       一帧之后自己恢复。症状与"porch/极性配错"的区别就在这里:
//       配错是**从头花到尾**,欠载是**只在写的那一下花**。
//
//   ★ 修法(在没有 bounce buffer 的前提下能做到的两件事,都做了):
//     ① **不欠载**:把写 fb 的时机挪到"帧结束中断之后"——那一刻正好是消隐期,
//        DMA 不读像素,CPU 独占 PSRAM;写完整帧才轮到有效像素。
//     ② **留足余量**:PCLK 从 Arduino 例程的 30MHz 降到 **18MHz**
//        —— 见第 3 块里的说明(18MHz 是官方 **ESP-IDF** 例程的值,那份例程的
//        bounce buffer 是**可选项**;Arduino 那份 30MHz 是**依赖** bounce buffer 的)。
//
//   ★ 代价(写清楚):每次 flush 会多等**一帧**(18MHz 下 15.5ms)。
//     我们的 UI 每 200ms 才重画一次 ⇒ 完全够用;开机动画那种整屏刷新会慢一点,
//     但那是"花屏"和"慢一帧"之间很划算的交换。
//   ★ 兜底:万一帧结束中断没来(面板没起来/被停),20ms 超时后照样写 ——
//     **绝不能在这里死等**(那会整屏再也不更新)。
//
//   ★★ 为什么"等"必须发生在**本函数里**(而不是先返回、过一帧再写):
//     LVGL 的刷新流程会在 `lv_timer_handler()` 里把这一帧画完并**等 `flush_ready`
//     清掉 `flushing`**才返回。所以"先 return、稍后再 flush_ready"会把
//     `lv_timer_handler()` 连同整个主循环**卡死**(实测:BEACON 停在
//     `step=7(loop: 数据已更新)` 再也不动,`rgb:` 那行一行都不打)。
//     在回调里同步等一帧是**标准做法**(LVGL 自己的 Linux fbdev 驱动就是
//     `ioctl(FBIOWAITVSYNC)` 阻塞等垂直同步),这里沿用同一条口径。
static uint32_t      g_sync_count = 0;       // 诊断:等到了帧结束才写的次数
static uint32_t      g_timeout_count = 0;    // 诊断:等超时(没等到中断)的次数
static uint32_t      g_skip_count = 0;       // 诊断:面板没建起来时直接放行的次数
static uint32_t      g_wait_us_max = 0;      // 诊断:最长等了多少 µs
static uint32_t      g_copy_us_max = 0;      // 诊断:单次 draw_bitmap 最长多少 µs
static uint32_t      g_copy_us_sum = 0;      // 诊断:累计写 fb 的 µs
static uint32_t      g_copy_n = 0;           // 诊断:写 fb 的次数
static uint32_t      g_chunk_count = 0;      // 诊断:被拆成几块写

// ★ 一次往 fb 里写多少**字节**。这个数是**实测倒推**的,不是拍的:
//   18MHz 下一帧 15.46ms,消隐期 = (508-480) 行 × 548 / 18e6 ≈ **0.85ms**;
//   而 `rgb_panel_draw_bitmap` 一边写 PSRAM、LCD_CAM 一边在读(36MB/s),
//   实测 19200 字节要 **1309µs**(≈14.7MB/s,一行 960B 的 memcpy 一笔一笔来),
//   已经超过消隐期 ⇒ 会溢进有效像素里,那一块就出现"上下半新旧两帧"的横缝。
//   8KB 按同一条实测速率约 **560µs**(占窗口 66%),留了三分之一余量。
//   ⇒ 每次只写一块,**每一块都落在自己的消隐期里**;块与块之间最多差一帧(15ms)。
#define RGB_FLUSH_MAX_BYTES 8192u

// 等到下一次"帧结束"(= 消隐期开始)。
//   ★ 必须是**微秒级**的等:消隐期只有 0.85ms,而 `delay(1)` 这种毫秒级轮询
//     最坏会晚 1ms 才醒 —— 那已经越过消隐期、扎进有效像素里了,撕裂就是这么来的
//     (2026-09-24:第一版用 delay(1) 自旋,花屏没了但**撕裂**还在,换成自旋后消掉)。
//   兜底:20ms 还没等到(面板没起来/被停)就照写,绝不死等。
static void wait_frame_boundary() {
  const uint32_t t0 = micros();
  while (!g_frame_done) {
    if ((uint32_t)(micros() - t0) >= 20000u) { ++g_timeout_count; return; }
  }
  g_frame_done = false;
  const uint32_t waited = (uint32_t)(micros() - t0);
  if (waited > g_wait_us_max) g_wait_us_max = waited;
  ++g_sync_count;
}

// LVGL → 面板:**等到帧结束(消隐期)再按块写进面板 fb**。
static void rgb_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  ++g_flush_count;
  if (g_panel == nullptr) {          // 没面板:直接放行,别把 LVGL 卡死
    ++g_skip_count;
    lv_display_flush_ready(disp);
    return;
  }

  // LVGL v9 的 px 指向绘制缓冲,区域内容按**区域自己的行距**紧排
  // (见 dash_display.cpp 预览驱动里那三条踩坑记录 —— 同一个坑),
  // 而 `rgb_panel_draw_bitmap` 要的正是"区域起点 + 区域行距"的指针 ⇒ 直接给;
  // 分块时按行数把源指针往后推。
  const uint32_t row_bytes = (uint32_t)(area->x2 - area->x1 + 1) * 2u;
  // 按**字节**定额算每次写几行(窄区域可以多写几行,宽区域少写几行)
  uint32_t rows_per_chunk = RGB_FLUSH_MAX_BYTES / (row_bytes ? row_bytes : 1u);
  if (rows_per_chunk == 0) rows_per_chunk = 1;

  for (int32_t y = area->y1; y <= area->y2; y += (int32_t)rows_per_chunk) {
    int32_t y2 = y + (int32_t)rows_per_chunk - 1;
    if (y2 > area->y2) y2 = area->y2;
    wait_frame_boundary();                       // ← 消隐期从这里开始
    const uint32_t c0 = micros();
    esp_lcd_panel_draw_bitmap(g_panel, area->x1, y, area->x2 + 1, y2 + 1,
                              px + (uint32_t)(y - area->y1) * row_bytes);
    const uint32_t dt = (uint32_t)(micros() - c0);
    g_copy_us_sum += dt;
    ++g_copy_n;
    ++g_chunk_count;
    if (dt > g_copy_us_max) g_copy_us_max = dt;
  }
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

// 背光:PWM 走 LEDC。★ 这块框架是 arduino-esp32 **2.0.17**,没有 3.x 的
// `ledcAttach(pin,freq,bits)` —— 2.x 是 ledcSetup(通道) + ledcAttachPin(脚) 两步。
// 例程用 20kHz / 10 位 / 50% 占空比,这里照抄(频率落在人耳外,不会听见啸叫)。
#define RGB_BL_LEDC_CH   1
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
  // ★ 18MHz 走 PLL160M(160/18≈8.89,分频器带小数部分,能凑准);
  //   30MHz 那一档才需要 PLL240M(240/8=30)。换了 pclk 记得一起换这一行。
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
  cfg.psram_trans_align = 64;
  cfg.sram_trans_align = 4;
  cfg.hsync_gpio_num = RGB_PIN_HSYNC;
  cfg.vsync_gpio_num = RGB_PIN_VSYNC;
  cfg.de_gpio_num = RGB_PIN_DE;
  cfg.pclk_gpio_num = RGB_PIN_PCLK;
  cfg.disp_gpio_num = -1;                 // 背光/显示使能另接 MOSFET(见 PINOUT)
  const int data_pins[16] = RGB_DATA_GPIOS;
  for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = data_pins[i];
  cfg.on_frame_trans_done = on_frame_trans_done;
  cfg.flags.fb_in_psram = 1;              // ★ 480×480×2B = 450KB,只能放 PSRAM

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &g_panel);
  if (err != ESP_OK || g_panel == nullptr) {
    // 不静默:黑屏时这一行是唯一线索
    dash_logf("rgb: 面板创建失败 err=%d(检查引脚/时序/PSRAM)\n", (int)err);
    g_panel = nullptr;
    return;
  }

  // ★★★ 这两行**不能省**:`esp_lcd_new_rgb_panel()` 只是把面板对象建起来,
  //   真正**开 DMA / 启动 LCD_CAM 连续扫描**的是 `esp_lcd_panel_init()`。
  //   少了它,面板一个像素都不发 —— 而症状极具误导性:
  //     · 串口一切正常、"rgb: 已就绪"照打、`draw_bitmap()` 也照抄进 fb;
  //     · 屏是**纯黑**;
  //     · 唯一能看出来的数字是每秒那行 `rgb: frames=0(+0/s)`
  //       (frames 由 EOF 中断里的回调累加,没扫描就永远是 0)。
  //   2026-09-24 第一次烧上 2.1 板时踩的正是这一条(骨架原稿漏了这两行,
  //   它是照 IDF 5.x 的习惯写的;官方例程 Display_ST7701.cpp 结尾有这两句)。
  //   `esp_lcd_panel_reset()` 对 RGB 面板是空操作(没有独立复位脚,
  //   复位走的是 EXIO1,前面已经拉过了),留着是为了与例程/iDF 文档一致。
  esp_lcd_panel_reset(g_panel);
  esp_lcd_panel_init(g_panel);

  // ---- 两个 lv_display:单屏版本先都画到同一块屏上 ----
  //   ★ 第二块屏到货后,把 g_right 换成它自己的 flush(见上面"双屏出路");
  //     现在这样至少能把"两块屏各自要显示什么"的 UI 逻辑先跑通。
  lv_display_t* d0 = lv_display_create(THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  lv_display_set_flush_cb(d0, rgb_flush_cb);
  lv_display_set_buffers(d0, g_draw_buf, nullptr, sizeof(g_draw_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  g_left = d0;
  g_right = d0;

  // ---- ③ 背光最后开 ----
  //   放在这里而不是开头,是为了"先有画面、再点亮":反过来的话,初始化那 600ms
  //   里屏是亮的但没内容(白/雪花),看起来像花屏,容易误判。
  ledcSetup(RGB_BL_LEDC_CH, RGB_BL_LEDC_HZ, RGB_BL_LEDC_BITS);
  ledcAttachPin(RGB_PIN_BL, RGB_BL_LEDC_CH);
  ledcWrite(RGB_BL_LEDC_CH, RGB_BL_DUTY);

  dash_logf("rgb: RGB565 %dx%d pclk=%uHz 数据位=%d 已就绪(第二块屏待接)\n",
                (int)THEME_DISPLAY_RES, (int)THEME_DISPLAY_RES,
                (unsigned)RGB_PIXEL_CLOCK_HZ, 16);
  dash_logf("rgb: 板=微雪 ESP32-S3-LCD-2.8C(非触控) ST7701 RST=EXIO1 CS=EXIO3 "
            "BL=GPIO%d/PWM%d @%u%%\n",
                (int)RGB_PIN_BL, (int)RGB_BL_LEDC_HZ,
                (unsigned)(RGB_BL_DUTY * 100u / (1u << RGB_BL_LEDC_BITS)));
}

lv_display_t* dash_display_left() { return g_left; }
lv_display_t* dash_display_right() { return g_right; }

// 每秒报一次帧率 —— 实屏调试时这是判断"面板到底在不在收帧"的第一手信息
// (和 VAN 那条 edges/frames 的诊断是同一个思路)。
//   sync/timeout 是"写 fb 有没有真的等到消隐期"的判据:
//   `sync` 跟着 flush 一起涨、`timeout` 一直是 0 ⇒ 同步在起作用。
void dash_display_poll() {
  static uint32_t last_ms = 0;
  static uint32_t last_frames = 0;
  static uint32_t last_sync = 0;
  static uint32_t last_copy_sum = 0;
  static uint32_t last_copy_n = 0;
  const uint32_t now = millis();
  if (now - last_ms < 1000) return;
  const uint32_t f = g_frames;
  const uint32_t dn = g_copy_n - last_copy_n;
  const uint32_t dsum = g_copy_us_sum - last_copy_sum;
  dash_logf("rgb: frames=%u(+%u/s) flush=%u sync=%u(+%u/s) timeout=%u "
            "chunk=%u copy_max=%uus copy_avg=%uus wait_max=%uus\n",
                (unsigned)f, (unsigned)(f - last_frames), (unsigned)g_flush_count,
                (unsigned)g_sync_count, (unsigned)(g_sync_count - last_sync),
                (unsigned)g_timeout_count, (unsigned)g_chunk_count,
                (unsigned)g_copy_us_max, (unsigned)(dn ? (dsum / dn) : 0u),
                (unsigned)g_wait_us_max);
  last_frames = f;
  last_sync = g_sync_count;
  last_copy_sum = g_copy_us_sum;
  last_copy_n = g_copy_n;
  last_ms = now;
}

#endif  // DASH_DISPLAY_RGB

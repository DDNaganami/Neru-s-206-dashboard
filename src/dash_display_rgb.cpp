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
#include <esp_heap_caps.h>

// ------------------------------------------------------------
// ★ 屏到手后**只改这个文件顶部的数字**,别处的代码不用动。
//   下面每一项都标了"从哪来",因为 ST7701S 的初始化时序/上电顺序各家不同,
//   抄错一项就是黑屏或者花屏(而且不报错)。
// ------------------------------------------------------------

// ---- 1) 引脚(待定:按 FPC 引脚定义 + 好布线排,原则见 PURCHASE 第十一节)----
//   ★ 16 根数据线尽量连号;避开 flash 26-32 / PSRAM 33-37 / USB 19-20 /
//     UART0 43-44 / VAN RX 16 / 引导脚 0-3-45-46。
//   下面是**占位值**,填之前先按 PURCHASE 第十一节的"可用池"核对一遍。
#define RGB_PIN_PCLK   4
#define RGB_PIN_HSYNC  5
#define RGB_PIN_VSYNC  6
#define RGB_PIN_DE     7
// 数据线顺序必须与屏的 FPC 定义一致(R0..R4, G0..G5, B0..B4 = 5+6+5)
#define RGB_DATA_GPIOS { 8, 9, 10, 11, 12, 13, 14, 15, \
                         21, 22, 23, 24, 25, 38, 39, 40 }

// ---- 2) 初始化命令(待填:从卖家例程/spec 里抄 ST770S 的上电序列)----
//   ★ 没有这一段就点不亮 —— 这是"RGB 屏比 SPI 屏难"的主要原因。
//   走 3 线 SPI(CS/SCLK/SDA)写命令,所以还要 3 个 GPIO。
#define RGB_PIN_INIT_CS    41
#define RGB_PIN_INIT_SCLK  42
#define RGB_PIN_INIT_SDA   47
// 下面这张表是**空占位**,格式照抄即可:{命令, {数据...}, 数据长度, 延时ms}
struct RgbInitCmd { uint8_t cmd; uint8_t data[16]; uint8_t len; uint16_t delay_ms; };
static const RgbInitCmd kPanelInit[] = {
  // 例:ST7701S 常见开头(★ 不要照这个用,等卖家资料)
  // { 0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0 },
  // { 0x36, {0x00}, 1, 0 },          // MADCTL:扫描方向
  // { 0x3A, {0x55}, 1, 0 },          // COLMOD:RGB565
  // { 0x11, {0}, 0, 120 },           // SLPOUT + 120ms
  // { 0x29, {0}, 0, 20 },            // DISPON
};
static const size_t kPanelInitCount = sizeof(kPanelInit) / sizeof(kPanelInit[0]);

// ---- 3) 时序(待核:按屏的 datasheet;下面这组是 480×480 常见值)----
//   像素时钟 = (h_res + hsync 前后沿) × (v_res + vsync 前后沿) × 刷新率。
//   480×480@60Hz 大约 16~18MHz —— 两块屏轮流发帧时要翻倍,见上面的说明。
#define RGB_PIXEL_CLOCK_HZ  (16 * 1000 * 1000)
#define RGB_HSYNC_PULSE     8
#define RGB_HSYNC_BACK      10
#define RGB_HSYNC_FRONT     20
#define RGB_VSYNC_PULSE     4
#define RGB_VSYNC_BACK      10
#define RGB_VSYNC_FRONT     10
#define RGB_PCLK_ACTIVE_NEG 0    // 有的屏在下降沿采样 → 改 1
#define RGB_HSYNC_IDLE_LOW  1
#define RGB_VSYNC_IDLE_LOW  1

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
static lv_color_t g_draw_buf[THEME_DISPLAY_RES * 40];

// 每帧转移完成:★ 这个回调在**中断上下文**里,只允许"记个数 / 翻个脚"这种动作,
// 绝不能在这里 memcpy(那正是 draw_bitmap 干的事,必须留给主循环)。
static bool IRAM_ATTR on_frame_trans_done(esp_lcd_panel_handle_t panel,
                                         esp_lcd_rgb_panel_event_data_t* edata,
                                         void* user_ctx) {
  (void)panel; (void)edata; (void)user_ctx;
  ++g_frames;
  return false;   // 没唤醒高优先级任务
}

// LVGL → 面板:只搬这一块区域(单屏版本就是这样,和 SPI 屏的 flush 一样)。
static void rgb_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  ++g_flush_count;
  if (g_panel != nullptr) {
    // ★ LVGL v9 的 px 指向绘制缓冲首地址,而区域内容按**区域自己的行距**紧排
    //   (见 dash_display.cpp 预览驱动里那三条踩坑记录 —— 同一个坑)。
    //   好在 esp_lcd_panel_draw_bitmap 要的是"区域起点 + 全屏行距"的指针,
    //   所以这里必须先按区域拷到一条临时行缓冲,或者把 LVGL 缓冲配成
    //   "区域起点 = 缓冲起点"(PARTIAL + 单区域刷新时成立)。
    //   → 目前直接用 LVGL 给的指针,若屏上出现错位残影,照预览驱动那样
    //     先按区域逐行拷进临时缓冲再交给面板(每行 aw*2 字节)。
    esp_lcd_panel_draw_bitmap(g_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px);
  }
  lv_display_flush_ready(disp);
}

// 写一条初始化命令(3 线 SPI 走 esp_lcd_panel_io)
static esp_lcd_panel_io_handle_t g_io = nullptr;

static void panel_init_sequence() {
  if (g_io == nullptr) return;
  for (size_t i = 0; i < kPanelInitCount; ++i) {
    const RgbInitCmd& c = kPanelInit[i];
    esp_lcd_panel_io_tx_param(g_io, c.cmd, c.len ? c.data : nullptr, c.len);
    if (c.delay_ms) delay(c.delay_ms);
  }
}

void dash_display_init() {
  // ---- 3 线 SPI:只用来写初始化命令,不进画 ----
  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.dc_gpio_num = -1;            // ST7701S 用 9 位 SPI(命令/数据靠第 9 位)
  io_cfg.cs_gpio_num = RGB_PIN_INIT_CS;
  io_cfg.pclk_hz = 10 * 1000 * 1000;
  io_cfg.lcd_cmd_bits = 9;
  io_cfg.lcd_param_bits = 9;
  io_cfg.spi_mode = 0;
  io_cfg.trans_queue_depth = 10;
  esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &g_io);
  panel_init_sequence();

  // ---- RGB 并口 ----
  esp_lcd_rgb_panel_config_t cfg = {};
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

  // ---- 两个 lv_display:单屏版本先都画到同一块屏上 ----
  //   ★ 第二块屏到货后,把 g_right 换成它自己的 flush(见上面"双屏出路");
  //     现在这样至少能把"两块屏各自要显示什么"的 UI 逻辑先跑通。
  lv_display_t* d0 = lv_display_create(THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  lv_display_set_flush_cb(d0, rgb_flush_cb);
  lv_display_set_buffers(d0, g_draw_buf, nullptr, sizeof(g_draw_buf),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  g_left = d0;
  g_right = d0;

  dash_logf("rgb: RGB565 %dx%d pclk=%uHz 数据位=%d 已就绪(第二块屏待接)\n",
                (int)THEME_DISPLAY_RES, (int)THEME_DISPLAY_RES,
                (unsigned)RGB_PIXEL_CLOCK_HZ, 16);
}

lv_display_t* dash_display_left() { return g_left; }
lv_display_t* dash_display_right() { return g_right; }

// 每秒报一次帧率 —— 实屏调试时这是判断"面板到底在不在收帧"的第一手信息
// (和 VAN 那条 edges/frames 的诊断是同一个思路)。
void dash_display_poll() {
  static uint32_t last_ms = 0;
  static uint32_t last_frames = 0;
  const uint32_t now = millis();
  if (now - last_ms < 1000) return;
  const uint32_t f = g_frames;
  dash_logf("rgb: frames=%u(+%u/s) flush=%u\n",
                (unsigned)f, (unsigned)(f - last_frames), (unsigned)g_flush_count);
  last_frames = f;
  last_ms = now;
}

#endif  // DASH_DISPLAY_RGB

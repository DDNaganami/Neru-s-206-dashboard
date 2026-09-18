// ============================================================
// 真实 SPI 屏驱动骨架(480×480,自带 GRAM 的那一族)
//
// 编译开关:`-DDASH_DISPLAY_SPI=1`(见 platformio.ini 的 [env:esp32s3-spi])。
// 与桩/预览/RGB 共用同一个接口(dash_display.h 的三个函数),dash_ui.cpp 一行未改。
//
// ------------------------------------------------------------
// ★ 为什么是"通用 SPI"而不是某个现成 panel 驱动(2026-09-18 查证)
//
// 这一版框架的 esp_lcd/include 下**只有**:
//     esp_lcd_panel_commands.h / io.h / ops.h / rgb.h / vendor.h
// —— **没有** st7789 / ili9341 / gc9a01 那些现成驱动。所以这里直接用
// `esp_lcd_panel_io_spi` 自己发命令 + 送像素。
// 这在本题里反而更对:RGB 屏和 SPI 屏都要"从卖家例程抄 init 序列",
// 我们本来就得自己维护那张表;而"写窗口(CASET/RASET) + RAMWR 送像素"
// 这一套覆盖了绝大多数 480×480 圆屏用的 IC(ST7789/ILI9341/GC9A01/NV3041 等)。
//
// ------------------------------------------------------------
// 两块屏怎么接(这就是 SPI 屏相对 RGB 屏的巨大优势)
//     共享:  SCK、MOSI          ← 2 根
//     每屏:  CS、DC、RST、BL    ← 4 根 × 2
//   合计 ≈ 10 根,S3 那排针绰绰有余,而且**不需要转接板**、飞线几十 MHz 也能跑。
//   (RGB 并口要 21 根 + 转接板,且这一版框架的双缓冲 API 还缺 —— 见 rgb 那个文件)
//
// 代价(务必知情,见 PURCHASE 第十一节):整屏刷新受 SPI 带宽限制 ——
//   480×480×16bit ≈ 3.7Mbit,80MHz 下单屏 ~27fps、两屏共用一条总线 ~13fps。
//   我们的 UI 日常是**局部刷新**(弧+表情+数字),够用;
//   开机/换背景这种整屏切换会慢一点 —— 这正好是"开机画面走程序化扫表"的又一个理由。
// ============================================================

#include "dash_display.h"
#include "ui_theme.h"
#include "dash_log.h"     // 日志同时打到 USB-CDC 与 UART0(见那个文件头)

#if defined(DASH_DISPLAY_SPI)

#include <Arduino.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>   // SPI2_HOST
#include <driver/ledc.h>         // 背光 PWM

// ------------------------------------------------------------
// ★ 屏到手后**只改这一段**。每个数都标了"从哪来",抄错一项就是黑屏/花屏,
//   而且不报错(SPI 屏没有"帧率不对"这种提示,只有画面不对)。
// ------------------------------------------------------------

// ---- 1) 引脚(占位值)■
//   ★ 已占用:26-32 flash / 33-37 OPI PSRAM / 19-20 USB-JTAG / 43-44 UART0 /
//     16 VAN RX;慎用 0/3/45/46(引导脚)。下面这些都在"可用池"里。
//   共享总线:
#define SPI_PIN_SCK   12
#define SPI_PIN_MOSI  11
#define SPI_HOST_ID   SPI2_HOST
#define SPI_CLOCK_HZ  (40 * 1000 * 1000)    // 先 40MHz;线长/花屏就降到 20/10MHz
//   每屏 4 根:[0]=左(转速) [1]=右(速度)
struct SpiPanelPins { int cs; int dc; int rst; int bl; };
static const SpiPanelPins kPanels[2] = {
  {10,  9,  8,  7 },    // 左屏
  {15, 14, 13, 21 },    // 右屏
};

// ---- 2) 初始化命令表(★ 待填:从卖家例程/spec 抄,不填点不亮)----
//   格式:{命令, {数据...}, 数据长度, 延时ms}。下面留了两条**常见**示例并注释掉,
//   不同 IC 的序列差别很大,别照抄 —— 但 0x11(SLPOUT)/0x29(DISPON) 几乎通用。
struct SpiInitCmd { uint8_t cmd; uint8_t data[16]; uint8_t len; uint16_t delay_ms; };
static const SpiInitCmd kPanelInit[] = {
  // 示例(★ 请替换成卖家给的那份):
  // { 0x01, {0}, 0, 120 },                    // SWRESET
  // { 0x3A, {0x55}, 1, 0 },                   // COLMOD = RGB565
  // { 0x36, {0x00}, 1, 0 },                   // MADCTL(扫描方向,花屏先改这里)
  // { 0x11, {0}, 0, 120 },                    // SLPOUT
  // { 0x29, {0}, 0, 20 },                     // DISPON
};
static const size_t kPanelInitCount = sizeof(kPanelInit) / sizeof(kPanelInit[0]);

// ---- 3) 窗口命令与偏移(不同 IC 见注释)----
//   ST7789/ILI9341/GC9A01/NV3041 都是 CASET=0x2A / RASET=0x2B / RAMWR=0x2C。
//   偏移:有些屏的可寻址区比可视区大(ST7789 是 240×320 里挖 240×240),
//        所以要在命令里加列/行偏移。480×480 的屏一般偏移为 0。
#define LCD_CMD_CASET  0x2A
#define LCD_CMD_RASET  0x2B
#define LCD_CMD_RAMWR  0x2C
#define SPI_COL_OFFSET 0
#define SPI_ROW_OFFSET 0

// ---- 4) 字节序 ----
//   ★ 多数 SPI 屏期望 RGB565 **大端**,而 LVGL 产出的是小端 —— 不交换就是
//     花屏/偏色(最经典的一个坑)。要交换就置 1(每帧多一次遍历,代价可接受)。
#define SPI_SWAP_RGB565 1

// ---- 5) 背光 ----
#define SPI_BL_LEDC_CH   LEDC_CHANNEL_0
#define SPI_BL_DUTY_PCT  80     // 上电默认亮度(夜里想调暗:见文件末尾 TODO)

// ------------------------------------------------------------
// 运行时
// ------------------------------------------------------------
static esp_lcd_panel_io_handle_t g_io[2] = {nullptr, nullptr};
static lv_display_t* g_disp[2] = {nullptr, nullptr};
static volatile uint32_t g_flush_count[2] = {0, 0};
static volatile uint32_t g_flush_bytes[2] = {0, 0};

// 颜色数据传输完成 → 告诉 LVGL 这块缓冲可以复用了(异步 flush 的标准做法)
static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t* edata,
                                    void* user_ctx) {
  (void)io; (void)edata;
  const int idx = (int)(intptr_t)user_ctx;
  if (idx >= 0 && idx < 2 && g_disp[idx] != nullptr) {
    lv_display_flush_ready(g_disp[idx]);
    return false;
  }
  return false;
}

// 硬复位(panel_io 不管 RST,得我们自己拉)
static void panel_reset(const SpiPanelPins& p) {
  pinMode(p.rst, OUTPUT);
  digitalWrite(p.rst, LOW);
  delay(20);
  digitalWrite(p.rst, HIGH);
  delay(120);   // 数据手册一般要求 ≥120ms 才能收命令
}

static void panel_send_init(esp_lcd_panel_io_handle_t io) {
  for (size_t i = 0; i < kPanelInitCount; ++i) {
    const SpiInitCmd& c = kPanelInit[i];
    esp_lcd_panel_io_tx_param(io, c.cmd, c.len ? c.data : nullptr, c.len);
    if (c.delay_ms) delay(c.delay_ms);
  }
}

// 画一块区域:写窗口 → 送像素。LVGL 的部分刷新天然就是"一块矩形",
// 所以这里和 SPI 屏的常规用法完全一致(不需要整屏缓冲)。
static void spi_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  const int idx = (int)(intptr_t)lv_display_get_user_data(disp);
  esp_lcd_panel_io_handle_t io = g_io[idx];
  if (io == nullptr) { lv_display_flush_ready(disp); return; }

  const int x1 = area->x1 + SPI_COL_OFFSET;
  const int x2 = area->x2 + SPI_COL_OFFSET;
  const int y1 = area->y1 + SPI_ROW_OFFSET;
  const int y2 = area->y2 + SPI_ROW_OFFSET;
  const uint8_t caset[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2 };
  const uint8_t raset[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2 };
  esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, caset, sizeof(caset));
  esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, raset, sizeof(raset));

  const size_t px_bytes = (size_t)(area->x2 - area->x1 + 1) * (size_t)(area->y2 - area->y1 + 1) * 2u;
#if SPI_SWAP_RGB565
  // RGB565 大小端交换(见上面第 4 条)。LVGL 提供现成的高效实现。
  lv_draw_sw_rgb565_swap(px, px_bytes / 2u);
#endif
  ++g_flush_count[idx];
  g_flush_bytes[idx] += (uint32_t)px_bytes;
  // 异步送:完成后由 on_color_done 调 lv_display_flush_ready
  esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, px, px_bytes);
}

// 两块屏各一条窄缓冲(放 SRAM):480×40×2B = 38KB/块,不占 PSRAM。
// ★ 用两块而不是共用一块:两块屏是**独立**刷新节奏(每屏各看各的数据),
//   共用一个 LVGL 绘制缓冲在 v9 里不允许(lv_display_set_buffers 会拒绝重复使用)。
static lv_color_t g_buf[2][THEME_DISPLAY_RES * 40];

void dash_display_init() {
  // ---- 共享总线 ----
  spi_bus_config_t bus = {};
  bus.sclk_io_num = SPI_PIN_SCK;
  bus.mosi_io_num = SPI_PIN_MOSI;
  bus.miso_io_num = -1;                       // 只写不读
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = THEME_DISPLAY_RES * 40 * 2;   // 一次 flush 的最大字节数
  const esp_err_t berr = spi_bus_initialize((spi_host_device_t)SPI_HOST_ID, &bus, SPI_DMA_CH_AUTO);
  if (berr != ESP_OK && berr != ESP_ERR_INVALID_STATE) {   // 已初始化过不算错
    dash_logf("spi: 总线初始化失败 err=%d(检查 SCK/MOSI 引脚)\n", (int)berr);
    return;
  }

  // ---- 两块屏 ----
  for (int i = 0; i < 2; ++i) {
    panel_reset(kPanels[i]);

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = kPanels[i].cs;
    io_cfg.dc_gpio_num = kPanels[i].dc;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = SPI_CLOCK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;      // 多数 SPI 屏:8 位命令 + 8 位参数
    io_cfg.lcd_param_bits = 8;
    io_cfg.on_color_trans_done = on_color_done;
    io_cfg.user_ctx = (void*)(intptr_t)i;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI_HOST_ID, &io_cfg, &g_io[i]) != ESP_OK) {
      dash_logf("spi: 第 %d 块屏 panel_io 创建失败(检查 CS/DC 引脚)\n", i);
      g_io[i] = nullptr;
      continue;
    }
    panel_send_init(g_io[i]);

    lv_display_t* d = lv_display_create(THEME_DISPLAY_RES, THEME_DISPLAY_RES);
    lv_display_set_user_data(d, (void*)(intptr_t)i);   // flush 里靠它找 io
    lv_display_set_flush_cb(d, spi_flush_cb);
    lv_display_set_buffers(d, g_buf[i], nullptr, sizeof(g_buf[i]),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    g_disp[i] = d;
  }

  // ---- 背光:PWM 而不是 GPIO 直驱(屏的背光 20~40mA,直驱会超 GPIO 上限)
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_8_BIT;
  t.timer_num = LEDC_TIMER_0;
  t.freq_hz = 5000;
  t.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&t);
  for (int i = 0; i < 2; ++i) {
    ledc_channel_config_t c = {};
    c.gpio_num = kPanels[i].bl;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel = (ledc_channel_t)(SPI_BL_LEDC_CH + i);
    c.timer_sel = LEDC_TIMER_0;
    c.duty = (uint32_t)(255 * SPI_BL_DUTY_PCT / 100);
    c.hpoint = 0;
    ledc_channel_config(&c);
  }

  dash_logf("spi: %dx%d 就绪(共享 SCK=%d MOSI=%d,左 CS=%d 右 CS=%d,pclk=%uHz)\n",
                (int)THEME_DISPLAY_RES, (int)THEME_DISPLAY_RES,
                SPI_PIN_SCK, SPI_PIN_MOSI, kPanels[0].cs, kPanels[1].cs,
                (unsigned)SPI_CLOCK_HZ);
  if (kPanelInitCount == 0) {
    // ★ 不静默:命令表是空的 → 屏一定不亮,而"不亮"的原因有十几种
    dash_logf("spi: ⚠ 初始化命令表为空(kPanelInit)—— 请从卖家例程抄进来,否则屏不会亮\n");
  }
}

lv_display_t* dash_display_left() { return g_disp[0]; }
lv_display_t* dash_display_right() { return g_disp[1]; }

// 每秒报一次刷新量与**实际字节吞吐** —— 实屏调试时用来回答
// "SPI 到底够不够用"(与 VAN 的 edges/frames 同一个思路)。
// 换算:字节/秒 ÷ 2 ÷ 480 ÷ 480 ≈ 等效整屏帧率。
void dash_display_poll() {
  static uint32_t last_ms = 0;
  static uint32_t last_flush[2] = {0, 0};
  static uint32_t last_bytes[2] = {0, 0};
  const uint32_t now = millis();
  if (now - last_ms < 1000) return;
  const uint32_t dt_ms = now - last_ms;
  char line[128];
  int n = 0;
  for (int i = 0; i < 2; ++i) {
    const uint32_t f = g_flush_count[i], b = g_flush_bytes[i];
    const uint32_t fps_equiv = (uint32_t)((uint64_t)1000u * (b - last_bytes[i]) /
                                          dt_ms / (THEME_DISPLAY_RES * THEME_DISPLAY_RES * 2u));
    n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%s flush=%u(+%u/s %uKB/s ~%ufps)",
                  i ? " | " : "", i == 0 ? "左" : "右",
                  (unsigned)f, (unsigned)(f - last_flush[i]),
                  (unsigned)((b - last_bytes[i]) / 1024u / (dt_ms / 1000u + 1u)),
                  (unsigned)fps_equiv);
    last_flush[i] = f;
    last_bytes[i] = b;
  }
  dash_logf("spi: %s\n", line);
  last_ms = now;
}

// TODO(以后):夜里想把屏调暗(与主题里的亮度档联动)就把这两个通道的 duty 暴露出去,
//   建议接口:void dash_display_set_backlight(uint8_t left_pct, uint8_t right_pct);
//   现在先固定 80%,等实屏在车上看过再定档位。

#endif  // DASH_DISPLAY_SPI

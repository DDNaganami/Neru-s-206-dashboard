#include "dash_display.h"
#include "ui_theme.h"   // 桩显示的宽高跟主题分辨率走,换屏只改 THEME_DISPLAY_RES

#if DASH_DISPLAY_STUB

// ============ 桩驱动:无实体屏,仅联调 ============
// esp32dev 内存紧张,用 分辨率x10 窄条缓冲;S3+PSRAM 后放大缓冲
static lv_color_t buf_left[THEME_DISPLAY_RES * 10];
static lv_color_t buf_right[THEME_DISPLAY_RES * 10];
static lv_display_t* g_left = nullptr;
static lv_display_t* g_right = nullptr;

static void stub_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  (void)disp;
  (void)area;
  (void)px;
  lv_display_flush_ready(disp);
}

void dash_display_init() {
  g_left = lv_display_create(THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  lv_display_set_flush_cb(g_left, stub_flush_cb);
  lv_display_set_buffers(g_left, buf_left, nullptr, sizeof(buf_left),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  g_right = lv_display_create(THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  lv_display_set_flush_cb(g_right, stub_flush_cb);
  lv_display_set_buffers(g_right, buf_right, nullptr, sizeof(buf_right),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
}

lv_display_t* dash_display_left() { return g_left; }
lv_display_t* dash_display_right() { return g_right; }

void dash_display_poll() {}   // 桩:无事可做

#elif defined(DASH_DISPLAY_PREVIEW)

// ============ 预览驱动:宿主机渲染,帧落盘,浏览器看动画 ============
// 用法:python -m platformio run -e pcpreview -t exec
//      跑几秒后 Ctrl+C,浏览器打开 preview/preview.html 看双屏扫表动画。
// 渲染走的是同一套 dash_ui/boot_anim/ui_theme,和真屏逻辑完全一致。
//
// ★ 2026-09-24:「2.8C(最终板)」档 —— 默认就把**圆屏可视区**标在落盘的帧上。
//   这块板是 480×480 的**圆屏**(微雪 ESP32-S3-LCD-2.8C,有效区 Ø70.13mm),
//   画布四角真机上根本看不见 ⇒ 预览必须把那个圆画出来,否则"素材伸到四角"
//   这类错在 PC 上看着好好的、装到表里才发现被圆边吃掉。
//   遮罩的几何在 lib/dashcore/panel_view.h(唯一一份,native 用例钉住),这里
//   只负责"把它落到落盘的那张图上"——LVGL 的渲染缓冲一个像素都不动,
//   所以帧里的可见区就是真机画面(不是"预览自己画了一版")。
//   按 `V` 可以开关这层遮罩(见 src/preview_input.h 的键表):
//   关掉之后落盘的就是**裸的画布**,想看四角里到底画了什么时用它。
//
// 踩坑记录(写实驱动前必读):
// 1) LVGL v9 的 lv_color_t 恒为 3 字节通用类型;flush 的 px 是显示格式的
//    原始像素(16 位=RGB565 / 32 位=XRGB8888),不能按 lv_color_t 拷贝。
// 2) flush 的 px 指向整屏绘制缓冲的**首地址**(lv_refr.c call_flush_cb),
//    但 PARTIAL 模式下区域内容按区域行距从缓冲起点紧排:
//    (y - area->y1) * aw + (x - area->x1)。按全屏偏移/行距拷贝会读错行。
// 3) 16 位色深下,任何 opa<255 的大对象(整屏淡入/半透明轨道)要开 ARGB8888
//    离屏层,层缓冲从绘制缓冲里切,装不下整屏 → 下半屏内容回绕到顶部。
//    预览已用独立绘制缓冲(见上),顶多画花;设备端别给屏幕对象设 opa<255,
//    实驱动按屏的实际格式推送 px 即可。
#include <stdio.h>
#include <string.h>
#include <direct.h>
#include <Arduino.h>   // millis()(预览桩)
#include "panel_view.h"   // 圆屏可视区几何（只在这个分支里用）
#include "flush_stats.h"  // 每秒"脏了多少"（与真机驱动同一份口径，见该文件头）

#if LV_COLOR_DEPTH == 32
typedef uint32_t fb_pixel_t;
static inline uint8_t px_r(fb_pixel_t c) { return (uint8_t)((c >> 16) & 0xFF); }
static inline uint8_t px_g(fb_pixel_t c) { return (uint8_t)((c >> 8) & 0xFF); }
static inline uint8_t px_b(fb_pixel_t c) { return (uint8_t)(c & 0xFF); }
#else
typedef uint16_t fb_pixel_t;
static inline uint8_t px_r(fb_pixel_t c) { return (uint8_t)(((c >> 11) & 0x1Fu) * 255u / 31u); }
static inline uint8_t px_g(fb_pixel_t c) { return (uint8_t)(((c >> 5) & 0x3Fu) * 255u / 63u); }
static inline uint8_t px_b(fb_pixel_t c) { return (uint8_t)((c & 0x1Fu) * 255u / 31u); }
#endif

// LVGL 绘制缓冲(渲染工作区)与帧缓冲(flush 落点)必须分开:
// PARTIAL 模式把区域内容紧排在绘制缓冲顶部,若绘制缓冲就是帧缓冲,
// 帧缓冲顶部会被"紧排内容"覆盖且再无 flush 修复(顶部白带/残影即此因)。
static fb_pixel_t draw_left[THEME_DISPLAY_RES * THEME_DISPLAY_RES];
static fb_pixel_t draw_right[THEME_DISPLAY_RES * THEME_DISPLAY_RES];
static fb_pixel_t fb_left[THEME_DISPLAY_RES * THEME_DISPLAY_RES];
static fb_pixel_t fb_right[THEME_DISPLAY_RES * THEME_DISPLAY_RES];
static lv_display_t* g_left = nullptr;
static lv_display_t* g_right = nullptr;
static uint32_t last_frame_ms = 0;
static uint32_t frame_no = 0;

// ---- 「2.8C(最终板)」档的那层遮罩（见上面文件头那一段）----
// 默认开。`V` 键可以关掉（关掉之后落盘的是裸画布）。
static bool g_panel_mask = true;
// 落帧计数（每次 poll 落一对帧 +1）。主循环用它判断"告警那一拍有没有被拍到"。
static uint32_t g_frames_written = 0;
static bool g_panel_banner_printed = false;

// ★ 每秒"脏了多少"（口径与真机驱动同一份：lib/dashcore/flush_stats.h）。
//   ★ 两份显示（左/右）**共用这一个累计器** ⇒ 打出来的和是"两屏合计"，
//     而占比按**单屏**面积算（两屏的 UI 一样重，合计 ≈ 单屏的 2 倍）。
static FlushStats g_fstats = {0u, 0u, 0, 0, 0u};
static uint32_t   g_fstats_last_ms = 0;
static uint32_t   g_fstats_flush_total = 0;

// 把遮罩落到一份 24bpp 的落盘副本上，**不动 LVGL 的缓冲**。
// ★ 为什么是"落盘的那一份"而不是原地改 fb：fb 是渲染的真值，改了就再也
//   回不去了（按 V 关掉遮罩之后屏幕内容已经被压暗过）。这里逐行现算，
//   每帧多一次 480 行的通道缩放 —— 宿主机上可以忽略（真机上不编这一段）。
static void write_bmp_panel(const char* path, const fb_pixel_t* fb, int32_t w, int32_t h) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  const uint32_t row = (w * 3u + 3u) & ~3u;
  const uint32_t data_size = row * (uint32_t)h;
  const uint32_t file_size = 54 + data_size;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  hdr[2] = (uint8_t)(file_size); hdr[3] = (uint8_t)(file_size >> 8);
  hdr[4] = (uint8_t)(file_size >> 16); hdr[5] = (uint8_t)(file_size >> 24);
  hdr[10] = 54;
  hdr[14] = 40;
  hdr[18] = (uint8_t)(w); hdr[19] = (uint8_t)(w >> 8);
  hdr[20] = (uint8_t)(w >> 16); hdr[21] = (uint8_t)(w >> 24);
  hdr[22] = (uint8_t)(h); hdr[23] = (uint8_t)(h >> 8);
  hdr[24] = (uint8_t)(h >> 16); hdr[25] = (uint8_t)(h >> 24);
  hdr[26] = 1; hdr[28] = 24;
  hdr[34] = (uint8_t)(data_size); hdr[35] = (uint8_t)(data_size >> 8);
  hdr[36] = (uint8_t)(data_size >> 16); hdr[37] = (uint8_t)(data_size >> 24);
  fwrite(hdr, 1, sizeof(hdr), f);
  // BMP 是自下而上存的；遮罩按"屏坐标"算，所以这里逐行反着走但坐标照常用。
  for (int32_t y = h - 1; y >= 0; --y) {
    for (int32_t x = 0; x < w; ++x) {
      fb_pixel_t c = fb[y * w + x];
      const uint16_t k = panelShadeAt(x, y, w);
      if (k < kPanelShadeInside) {
#if LV_COLOR_DEPTH == 32
        uint8_t r = px_r(c), g = px_g(c), b = px_b(c);
        r = (uint8_t)(((uint32_t)r * k + kPanelShadeInside / 2u) / kPanelShadeInside);
        g = (uint8_t)(((uint32_t)g * k + kPanelShadeInside / 2u) / kPanelShadeInside);
        b = (uint8_t)(((uint32_t)b * k + kPanelShadeInside / 2u) / kPanelShadeInside);
        c = ((fb_pixel_t)r << 16) | ((fb_pixel_t)g << 8) | (fb_pixel_t)b;
#else
        // 16 位 RGB565：与 panel_view.h 的 panelApplyOverlayRgb565 **同一套**
        // 通道缩放（这里手写一遍是为了就地复用 px_b/px_g/px_r，别无第二套口径）。
        const uint8_t r = (uint8_t)((c >> 11) & 0x1Fu);
        const uint8_t g = (uint8_t)((c >> 5) & 0x3Fu);
        const uint8_t b = (uint8_t)(c & 0x1Fu);
        c = (fb_pixel_t)((panelScaleRgb565Channel(r, k) << 11) |
                         (panelScaleRgb565Channel(g, k) << 5) |
                          panelScaleRgb565Channel(b, k));
#endif
      }
      fputc(px_b(c), f);
      fputc(px_g(c), f);
      fputc(px_r(c), f);
    }
    for (uint32_t p = w * 3u; p < row; ++p) fputc(0, f);
  }
  fclose(f);
}

static void preview_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  fb_pixel_t* fb = (fb_pixel_t*)lv_display_get_user_data(disp);
  const uint32_t w = (uint32_t)lv_display_get_horizontal_resolution(disp);
  const uint32_t aw = (uint32_t)(area->x2 - area->x1 + 1);
  // ★ 每秒"脏了多少"（与真机驱动同一份口径/同一个头，见 flush_stats.h）。
  //   预览的绘制缓冲是**整屏大小** ⇒ 这里拿到的是 LVGL 真正标脏的那些区域，
  //   没有真机那条"16 行绘制缓冲"的上限 ⇒ **单次最大矩形能直接暴露整屏失效**。
  flush_stats_add(g_fstats, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
  // v9 PARTIAL 模式:px 指向绘制缓冲**首地址**,但区域内容按区域自己的行距
  // (aw)从缓冲起点紧排,即 (y - area->y1) * aw + (x - area->x1) ——
  // 不是"全屏行距 + 全屏偏移"(那会让窄区域读错行,屏上出现错位残影)。
  const fb_pixel_t* src = (const fb_pixel_t*)px;
  for (int32_t y = area->y1; y <= area->y2; ++y) {
    memcpy(&fb[y * w + area->x1], src, aw * sizeof(fb_pixel_t));
    src += aw;
  }
  lv_display_flush_ready(disp);
}

// 显示格式像素 → 24bpp BGR,BMP(行 4 字节对齐,自下而上)
static void write_bmp(const char* path, const fb_pixel_t* fb, int32_t w, int32_t h) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  const uint32_t row = (w * 3u + 3u) & ~3u;
  const uint32_t data_size = row * (uint32_t)h;
  const uint32_t file_size = 54 + data_size;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  hdr[2] = (uint8_t)(file_size); hdr[3] = (uint8_t)(file_size >> 8);
  hdr[4] = (uint8_t)(file_size >> 16); hdr[5] = (uint8_t)(file_size >> 24);
  hdr[10] = 54;
  hdr[14] = 40;
  hdr[18] = (uint8_t)(w); hdr[19] = (uint8_t)(w >> 8);
  hdr[20] = (uint8_t)(w >> 16); hdr[21] = (uint8_t)(w >> 24);
  hdr[22] = (uint8_t)(h); hdr[23] = (uint8_t)(h >> 8);
  hdr[24] = (uint8_t)(h >> 16); hdr[25] = (uint8_t)(h >> 24);
  hdr[26] = 1; hdr[28] = 24;
  hdr[34] = (uint8_t)(data_size); hdr[35] = (uint8_t)(data_size >> 8);
  hdr[36] = (uint8_t)(data_size >> 16); hdr[37] = (uint8_t)(data_size >> 24);
  fwrite(hdr, 1, sizeof(hdr), f);
  for (int32_t y = h - 1; y >= 0; --y) {
    for (int32_t x = 0; x < w; ++x) {
      const fb_pixel_t c = fb[y * w + x];
      fputc(px_b(c), f);
      fputc(px_g(c), f);
      fputc(px_r(c), f);
    }
    for (uint32_t p = w * 3u; p < row; ++p) fputc(0, f);
  }
  fclose(f);
}

void dash_display_init() {
  _mkdir("preview");
  _mkdir("preview/frames");
  g_left = lv_display_create(THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  lv_display_set_user_data(g_left, fb_left);
  lv_display_set_flush_cb(g_left, preview_flush_cb);
  lv_display_set_buffers(g_left, draw_left, nullptr, sizeof(draw_left),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  g_right = lv_display_create(THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  lv_display_set_user_data(g_right, fb_right);
  lv_display_set_flush_cb(g_right, preview_flush_cb);
  lv_display_set_buffers(g_right, draw_right, nullptr, sizeof(draw_right),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
}

lv_display_t* dash_display_left() { return g_left; }
lv_display_t* dash_display_right() { return g_right; }

// 蜂鸣器那一侧的"落一行"出口（lib/dashcore/buzzer.cpp 声明它、这里给实现）。
// ★ 为什么挂在**这个**文件而不是新开一个：它要的只是"把一行文本送到
//   运行预览那个终端"，而预览驱动这一段已经拿着 stdout 与 Arduino 桩；
//   单独开一个 .cpp 只会让"pcpreview 到底编了哪些文件"更难数。
// ★ native 构建里同一符号由 buzzer.cpp 自己的默认实现提供（fputs 到 stdout），
//   两边签名一致 —— 签名对不上时 native 会直接链接失败（那是刻意的）。
void buzzer_host_printf(const char* line) {
  fputs(line, stdout);
  fputc('\n', stdout);
  fflush(stdout);   // ★ 不 flush 的话这一行会卡在缓冲里，"按了怎么没反应"最难查
}

// ---- 「2.8C(最终板)」档：遮罩的运行时开关 + 帧号（见文件头）----
void dash_display_preview_set_panel_mask(bool on) { g_panel_mask = on; }
bool dash_display_preview_panel_mask() { return g_panel_mask; }
uint32_t dash_display_preview_frames() { return g_frames_written; }

// ---- ★★ 预览专用的"钉住开机窗口"（验开机角色标签用，见 dash_display.h 那段）----
// ★ 默认 false ⇒ **不设 `hold=` 时行为与以前逐帧相同**（这条很重要：交付的预览
//   回归跑的就是默认那一档）。
// ★ 它只影响**标签的可见性判据**，不影响 `g_boot` 本身 —— 扫表/表情那套动画
//   照旧按 `BOOT_TOTAL_MS` 走完，一趟都不多跑（"不许干扰既有的开机动画"那条）。
static bool g_boot_hold = false;
void dash_display_preview_set_boot_hold(bool on) { g_boot_hold = on; }
bool dash_display_preview_boot_hold() { return g_boot_hold; }

// 开机 banner：把"这块屏是圆的、可视圆是多少"打进日志。
// ★ 一次性：它在报告里是可以直接引用的一行（"这轮跑的是 2.8C 档"）。
// ★ 纯 ASCII：README 那条纪律（中文在 GBK 控制台上会抛 UnicodeEncodeError）。
static void print_panel_banner_once() {
  if (g_panel_banner_printed) return;
  g_panel_banner_printed = true;
  char line[192];
  panelDescribe(line, sizeof(line), THEME_DISPLAY_RES);
  printf("preview: %s\n", line);
  printf("preview: round mask %s (key V toggles; frames are 480x480 with the mask burned into the BMP)\n",
         g_panel_mask ? "ON" : "OFF");
  fflush(stdout);
}

// 每 200ms 落一对 BMP(共 150 对 = 30 秒,够看开机动画 + 假数据走动)
void dash_display_poll() {
  // ★ 每秒那两行"脏了多少"（**在落帧的早退之前**：30 秒之后不再落帧，
  //   但"脏了多少"这条诊断仍然要能一直打 —— 它是给"数字在跳"那种场景用的）。
  const uint32_t now_ms = millis();
  if (g_fstats_last_ms == 0) g_fstats_last_ms = now_ms;
  if (now_ms - g_fstats_last_ms >= 1000) {
    g_fstats_last_ms = now_ms;
    // 累计值在**这一秒结算时**加一次（口径与真机那行的 `累计%u` 一致：
    // 都含当前这一秒）。
    g_fstats_flush_total += g_fstats.n;
    const uint32_t scr_px = (uint32_t)THEME_DISPLAY_RES * (uint32_t)THEME_DISPLAY_RES;
    const uint32_t inv_pct = flush_stats_pct_x10(g_fstats.area_sum, scr_px);
    const uint32_t fmax_pct = flush_stats_pct_x10(g_fstats.max_area, scr_px);
    printf("preview: 脏区/s(左右两屏合计) inv=%upx2(=单屏的 %u.%u%%) flush=%u/s(累计%u) "
           "fmax=%dx%d(单屏的 %u.%u%%) 单屏=%upx2\n",
           (unsigned)g_fstats.area_sum,
           (unsigned)(inv_pct / 10u), (unsigned)(inv_pct % 10u),
           (unsigned)g_fstats.n, (unsigned)g_fstats_flush_total,
           (int)g_fstats.max_w, (int)g_fstats.max_h,
           (unsigned)(fmax_pct / 10u), (unsigned)(fmax_pct % 10u),
           (unsigned)scr_px);
    fflush(stdout);
    flush_stats_reset(g_fstats);
  }

  if (frame_no >= 150) return;
  if (millis() - last_frame_ms < 200) return;
  last_frame_ms = millis();
  print_panel_banner_once();
  char path[64];
  snprintf(path, sizeof(path), "preview/frames/l_%04u.bmp", frame_no);
  // ★ 开关决定**落盘的那一份**要不要压暗四角；LVGL 的 fb 一个像素都不动。
  if (g_panel_mask) write_bmp_panel(path, fb_left, THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  else              write_bmp(path, fb_left, THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  snprintf(path, sizeof(path), "preview/frames/r_%04u.bmp", frame_no);
  if (g_panel_mask) write_bmp_panel(path, fb_right, THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  else              write_bmp(path, fb_right, THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  ++frame_no;
  g_frames_written = frame_no;   // 主循环读它判断"告警那一拍被拍到没有"
}

#elif defined(DASH_DISPLAY_SPI)

// ============ 实驱动:SPI 屏(实现见 dash_display_spi.cpp) ============
// 两块屏共享 SCK/MOSI、各自 CS/DC/RST/BL ≈ 10 根,不需要转接板。
// ★ 这一版框架**没有**现成的 st7789/ili9341 驱动(esp_lcd/include 下只有
//   commands/io/ops/rgb/vendor),所以那边是"自己发命令 + 送像素"的通用实现,
//   init 序列表要从卖家例程抄。编译开关见 [env:esp32s3-spi]。
#elif defined(DASH_DISPLAY_RGB)

// ============ 实驱动:RGB 并口屏(实现见 dash_display_rgb.cpp) ============
// 单独一个文件,免得这个文件被两套实现撑成五百行。
// 它提供**同一组**接口(dash_display_init / left / right / poll),
// 所以 dash_ui.cpp 一行都不用改 —— 这正是当初抽接口的目的。
// 编译开关见 platformio.ini 的 [env:esp32s3-rgb]。
// ★ 双屏方案被这一版框架的旧 esp_lcd 卡住(缺 num_fbs/get_frame_buffer),
//   原因与两条出路写在该文件的文件头,先读那段再动手。

#else

// ============ 实驱动:屏到货后在此实现(ST7701S / GC9xxx SPI / LovyanGFX) ============
// 顺序:1) 选屏锁分辨率 → 改 ui_theme.h 的 THEME_DISPLAY_RES
//       2) 板子必须是 ESP32-S3 + PSRAM(480×480 双屏 esp32dev 内存不够)
//       3) 在这里注册两块真屏的 flush_cb 与缓冲
//       4) 最后才删 -DDASH_DISPLAY_STUB
// 只删宏不写驱动 → 编译直接报错,避免上电黑屏空指针。
#error "dash_display 实驱动尚未实现:先按上面步骤写驱动,再移除 DASH_DISPLAY_STUB"

#endif

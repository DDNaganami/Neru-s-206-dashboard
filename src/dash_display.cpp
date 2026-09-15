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

static void preview_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  fb_pixel_t* fb = (fb_pixel_t*)lv_display_get_user_data(disp);
  const uint32_t w = (uint32_t)lv_display_get_horizontal_resolution(disp);
  const uint32_t aw = (uint32_t)(area->x2 - area->x1 + 1);
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

// 每 200ms 落一对 BMP(共 150 对 = 30 秒,够看开机动画 + 假数据走动)
void dash_display_poll() {
  if (frame_no >= 150) return;
  if (millis() - last_frame_ms < 200) return;
  last_frame_ms = millis();
  char path[64];
  snprintf(path, sizeof(path), "preview/frames/l_%04u.bmp", frame_no);
  write_bmp(path, fb_left, THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  snprintf(path, sizeof(path), "preview/frames/r_%04u.bmp", frame_no);
  write_bmp(path, fb_right, THEME_DISPLAY_RES, THEME_DISPLAY_RES);
  ++frame_no;
}

#else

// ============ 实驱动:屏到货后在此实现(ST7701S / GC9xxx SPI / LovyanGFX) ============
// 顺序:1) 选屏锁分辨率 → 改 ui_theme.h 的 THEME_DISPLAY_RES
//       2) 板子必须是 ESP32-S3 + PSRAM(480×480 双屏 esp32dev 内存不够)
//       3) 在这里注册两块真屏的 flush_cb 与缓冲
//       4) 最后才删 -DDASH_DISPLAY_STUB
// 只删宏不写驱动 → 编译直接报错,避免上电黑屏空指针。
#error "dash_display 实驱动尚未实现:先按上面步骤写驱动,再移除 DASH_DISPLAY_STUB"

#endif

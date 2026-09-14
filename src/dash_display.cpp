#include "dash_display.h"

#if DASH_DISPLAY_STUB

// ============ 桩驱动:无实体屏,仅联调 ============
// esp32dev 内存紧张,用 480x10 窄条缓冲;S3+PSRAM 后放大缓冲
static lv_color_t buf_left[480 * 10];
static lv_color_t buf_right[480 * 10];
static lv_display_t* g_left = nullptr;
static lv_display_t* g_right = nullptr;

static void stub_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  (void)disp;
  (void)area;
  (void)px;
  lv_display_flush_ready(disp);
}

void dash_display_init() {
  g_left = lv_display_create(480, 480);
  lv_display_set_flush_cb(g_left, stub_flush_cb);
  lv_display_set_buffers(g_left, buf_left, nullptr, sizeof(buf_left),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  g_right = lv_display_create(480, 480);
  lv_display_set_flush_cb(g_right, stub_flush_cb);
  lv_display_set_buffers(g_right, buf_right, nullptr, sizeof(buf_right),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
}

lv_display_t* dash_display_left() { return g_left; }
lv_display_t* dash_display_right() { return g_right; }

#else

// ============ 实驱动:TODO 屏到货后填 ============
void dash_display_init() {}
lv_display_t* dash_display_left() { return nullptr; }
lv_display_t* dash_display_right() { return nullptr; }

#endif

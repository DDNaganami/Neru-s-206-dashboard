#pragma once
#include <lvgl.h>

// ============================================================
// 主题配置 —— 换皮只改这个文件
// 设计定案:无指针;外圈=圆弧进度条(车速/转速/水温);中间=角色表情
// ============================================================

// ---- 全局 ----
static const lv_color_t THEME_BG_COLOR = lv_color_hex(0x141414);  // 深底

// ---- 分辨率适配 ----
// 所有尺寸/位置常量按 480×480 基准设计;换屏只改 THEME_DISPLAY_RES,
// 渲染时统一乘 theme_scale()。例:3.4" 圆屏 800×800 → 填 800。
// 桩驱动直接读这个常量注册显示宽高,两处永远一致。
// 注意:480×480 双屏真驱动必须 ESP32-S3 + PSRAM(见 ARCHITECTURE.md)。
static const int32_t THEME_BASE_RES    = 480;
static const int32_t THEME_DISPLAY_RES = 480;
static inline float theme_scale() {
  return (float)THEME_DISPLAY_RES / (float)THEME_BASE_RES;
}

static const uint16_t THEME_FACE_SIZE = 200;  // 表情区域边长(480 基准)

// ---- 开机动画(ms):淡入 → 双屏错峰扫表 → 表情睁眼 ----
static const uint32_t BOOT_FADE_MS        = 250;    // 全屏淡入
static const uint32_t BOOT_SWEEP_START_MS = 250;    // 扫表开始
static const uint32_t BOOT_STAGGER_MS     = 100;    // 右屏错峰
static const uint32_t BOOT_SWEEP_RISE_MS  = 380;    // 0 → 满弧
static const uint32_t BOOT_SWEEP_HOLD_MS  = 140;    // 满弧停留
static const uint32_t BOOT_SWEEP_FALL_MS  = 420;    // 满弧 → 回零
static const uint32_t BOOT_FACE_START_MS  = 1000;   // 表情开始睁眼
static const uint32_t BOOT_FACE_BLINK_MS  = 130;    // 一次眨眼
static const uint32_t BOOT_TOTAL_MS =
    BOOT_FACE_START_MS + 4 * BOOT_FACE_BLINK_MS;

// 正常渲染弧值缓动:指数趋近速率(1/s),与渲染频率解耦。
// 换算自旧实现 kArcSmooth=0.25/每 200ms 档:0.75^5≈0.237/s → 速率 -ln(0.237)≈1.44
static const float kArcSmoothPerSec = 1.44f;

// ---- 圆弧定义 ----
enum class ArcKind : uint8_t { Speed, Rpm, Coolant };

struct ArcStyle {
  ArcKind kind;
  int32_t start_deg;   // 0°=3点钟,顺时针;135→405 即 270° 范围、12 点上方开口
  int32_t end_deg;
  int32_t radius;      // 弧半径(480 基准,渲染时乘 theme_scale())
  int32_t width;       // 弧线宽(480 基准)
  lv_color_t track_color;   // 未点亮轨道
  uint8_t    track_opa;
  lv_color_t value_color;   // 已点亮部分
};

// 左屏(原车速位):车速弧 + 水温弧
static const ArcStyle kLeftArcs[] = {
  { ArcKind::Speed,   135, 405, 205, 24, lv_color_hex(0x232323), LV_OPA_60, lv_color_hex(0x39C5FF) },
  { ArcKind::Coolant, 145, 330, 168, 10, lv_color_hex(0x232323), LV_OPA_60, lv_color_hex(0x7CFF6B) },
};
// 右屏(原转速位):转速弧
static const ArcStyle kRightArcs[] = {
  { ArcKind::Rpm,     135, 405, 205, 24, lv_color_hex(0x232323), LV_OPA_60, lv_color_hex(0xFF5C5C) },
};

static const uint8_t kMaxArcs = 3;

struct ScreenTheme {
  const ArcStyle* arcs;
  uint8_t arc_count;
  bool show_face;
};
static const ScreenTheme kScreens[2] = {
  // arc_count 用 sizeof 自动算,避免改表后忘改计数 → 越界读到垃圾弧参数
  { kLeftArcs,  (uint8_t)(sizeof(kLeftArcs)  / sizeof(kLeftArcs[0])),  true  },  // 左屏:车速+水温弧 + 表情
  { kRightArcs, (uint8_t)(sizeof(kRightArcs) / sizeof(kRightArcs[0])), true  },  // 右屏:转速弧 + 表情
};

// ---- 量程映射 ----
static const float kCoolantMinC = 60.0f;
static const float kCoolantMaxC = 130.0f;

// ============================================================
// 占位表情(形状组合,演示表情切换)。位置/尺寸同为 480 基准。
// 换真实角色图时:把 dash_ui.cpp 的 face_apply() 整体替换为
// lv_image 加载图片数组(ui_assets.h 放角色图),主题常量一并移过去
// ============================================================
static const lv_color_t FACE_BG_IDLE    = lv_color_hex(0xFFFFFF);
static const lv_color_t FACE_BG_REDLINE = lv_color_hex(0xFF4D4D);
static const lv_color_t FACE_INK        = lv_color_hex(0x1A1A1A);

static const uint8_t FACE_EYE_L_X = 38;
static const uint8_t FACE_EYE_R_X = 136;
static const uint8_t FACE_EYE_Y   = 66;
static const uint8_t EYE_NORMAL_W = 26;
static const uint8_t EYE_NORMAL_H = 26;
static const uint8_t EYE_SURPRISE = 44;   // 惊喜:大圆眼
static const uint8_t EYE_NARROW_H = 8;    // 巡航/运动/红区:眯眼

static const uint8_t MOUTH_LINE_X = 75;
static const uint8_t MOUTH_LINE_Y = 138;
static const uint8_t MOUTH_LINE_W = 50;
static const uint8_t MOUTH_LINE_H = 6;
static const uint8_t MOUTH_O_X = 80;
static const uint8_t MOUTH_O_Y = 120;
static const uint8_t MOUTH_O_SIZE = 40;   // 惊喜:"O" 嘴

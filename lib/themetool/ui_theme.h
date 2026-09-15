#pragma once
#include <lvgl.h>
#include <stdint.h>

// ============================================================
// 主题 —— 换皮只改这个文件（编译期默认值）
//
// 设计定案:无指针;外圈=圆弧进度条(车速/转速/水温);中间=角色表情
//
// ★ 运行时主题（2026-09-15 新增）
//   原来所有值都是 `static const`,改一个颜色就得重编译重刷。现在改成:
//     1. ui_theme.h 只提供**默认值**（Theme 结构体 + kDefaultTheme）
//     2. 启动时 theme_load() 尝试从 flash 的 theme 分区读主题文件
//        （见 theme_store.h）。读不到就用默认值 —— 不刷主题也能跑
//     3. 代码里仍写 THEME_BG_COLOR 这样的名字不变,它们是**转发宏**,
//        展开成 "从全局主题里取这个字段"
//   这样改配色/几何只需重新生成主题文件并刷 theme 分区,**固件不用重编译**。
//
// 编辑工具:tools/theme-editor/（本地网页,可视化改配色与几何、实时预览、导出主题文件）
//
// 注意:THEME_DISPLAY_RES 是唯一保留为**编译期常量**的项 —— 它决定显示缓冲
//      尺寸,必须编译期确定（换屏才改它）。
// ============================================================

// ---- 分辨率（编译期） ----
// 所有尺寸/位置按 480×480 基准设计;渲染时统一乘 theme_scale()。
// 桩驱动直接读这个常量注册显示宽高,两处永远一致。
// 注意:480×480 双屏真驱动必须 ESP32-S3 + PSRAM(见 ARCHITECTURE.md)。
#define THEME_BASE_RES     480
#ifndef THEME_DISPLAY_RES
#define THEME_DISPLAY_RES  480
#endif
static inline float theme_scale() {
  return (float)THEME_DISPLAY_RES / (float)THEME_BASE_RES;
}

// ---- 圆弧定义 ----
enum class ArcKind : uint8_t { Speed, Rpm, Coolant };
static const uint8_t kMaxArcs = 3;

struct ArcStyle {
  ArcKind  kind;
  int32_t  start_deg;  // 0°=3点钟,顺时针;135→405 即 270° 范围、12 点上方开口
  int32_t  end_deg;
  int32_t  radius;     // 弧半径(480 基准,渲染时乘 theme_scale())
  int32_t  width;      // 弧线宽(480 基准)
  lv_color_t track_color;  // 未点亮轨道
  uint8_t  track_opa;      // 0..255
  lv_color_t value_color;  // 已点亮部分
};

struct ScreenTheme {
  ArcStyle arcs[kMaxArcs];
  uint8_t  arc_count;
  uint8_t  show_face;
};

// ---- 主题数据（全部运行时可改） ----
// 字段顺序即序列化顺序,改动要在 theme_store.cpp 里同步（并升版本号）。
struct Theme {
  // 全局 / 表情
  uint32_t bg_color;        // 屏幕底色
  int32_t  face_size;       // 表情区域边长(480 基准)
  uint32_t face_bg_idle;    // 表情底色（常态）
  uint32_t face_bg_redline; // 表情底色（红区）
  uint32_t face_ink;        // 五官颜色

  // 五官几何(480 基准)
  uint8_t eye_l_x, eye_r_x, eye_y;
  uint8_t eye_normal_w, eye_normal_h;
  uint8_t eye_surprise;     // 惊喜:大圆眼
  uint8_t eye_narrow_h;     // 巡航/运动/红区:眯眼
  uint8_t mouth_line_x, mouth_line_y, mouth_line_w, mouth_line_h;
  uint8_t mouth_o_x, mouth_o_y, mouth_o_size;

  // 量程
  float coolant_min_c, coolant_max_c;

  // 开机动画(ms):淡入 → 双屏错峰扫表 → 表情睁眼
  uint32_t boot_fade_ms;
  uint32_t boot_sweep_start_ms;
  uint32_t boot_stagger_ms;
  uint32_t boot_sweep_rise_ms;
  uint32_t boot_sweep_hold_ms;
  uint32_t boot_sweep_fall_ms;
  uint32_t boot_face_start_ms;
  uint32_t boot_face_blink_ms;
  uint32_t boot_total_ms;      // 由上面几个推导,加载时重算

  // 两屏的弧
  ScreenTheme screens[2];
};

// 正常渲染弧值缓动:指数趋近速率(1/s),与渲染频率解耦。
// 换算自旧实现 kArcSmooth=0.25/每 200ms 档:0.75^5≈0.237/s → 速率 -ln(0.237)≈1.44
static const float kArcSmoothPerSec = 1.44f;

// 编译期默认主题（与 2026-09 的原始值完全一致）
inline void theme_set_defaults(Theme& t) {
  t.bg_color        = 0x141414;
  t.face_size       = 200;
  t.face_bg_idle    = 0xFFFFFF;
  t.face_bg_redline = 0xFF4D4D;
  t.face_ink        = 0x1A1A1A;

  t.eye_l_x = 38;  t.eye_r_x = 136; t.eye_y = 66;
  t.eye_normal_w = 26; t.eye_normal_h = 26;
  t.eye_surprise = 44; t.eye_narrow_h = 8;
  t.mouth_line_x = 75; t.mouth_line_y = 138; t.mouth_line_w = 50; t.mouth_line_h = 6;
  t.mouth_o_x = 80;    t.mouth_o_y = 120;    t.mouth_o_size = 40;

  t.coolant_min_c = 60.0f;
  t.coolant_max_c = 130.0f;

  t.boot_fade_ms        = 250;
  t.boot_sweep_start_ms = 250;
  t.boot_stagger_ms     = 100;
  t.boot_sweep_rise_ms  = 380;
  t.boot_sweep_hold_ms  = 140;
  t.boot_sweep_fall_ms  = 420;
  t.boot_face_start_ms  = 1000;
  t.boot_face_blink_ms  = 130;
  t.boot_total_ms       = t.boot_face_start_ms + 4 * t.boot_face_blink_ms;

  // 左屏(原车速位):车速弧 + 水温弧
  ScreenTheme& L = t.screens[0];
  L.arc_count = 2; L.show_face = 1;
  L.arcs[0] = ArcStyle{ ArcKind::Speed,   135, 405, 205, 24,
                        lv_color_hex(0x232323), 153, lv_color_hex(0x39C5FF) };
  L.arcs[1] = ArcStyle{ ArcKind::Coolant, 145, 330, 168, 10,
                        lv_color_hex(0x232323), 153, lv_color_hex(0x7CFF6B) };

  // 右屏(原转速位):转速弧
  ScreenTheme& R = t.screens[1];
  R.arc_count = 1; R.show_face = 1;
  R.arcs[0] = ArcStyle{ ArcKind::Rpm,     135, 405, 205, 24,
                        lv_color_hex(0x232323), 153, lv_color_hex(0xFF5C5C) };
}

// 全局主题**指针**。
//
// 为什么是指针而不是可写实例（重要，别改回实例）：
//   主题在运行期是**只读**的 —— LVGL 只把里面的颜色/尺寸读出去用，
//   不需要一份可写的 DRAM 副本。所以:
//     · 默认主题是 const,放 flash(.rodata)
//     · 从 flash 主题分区加载成功后,把指针指到解析结果
//   实测(esp32dev,320KB DRAM):把主题做成**可写全局实例**会让 LVGL 多占
//   约 36KB —— 颜色不再是编译期常量后 LVGL 无法把样式对象折叠掉。
//   改回实例会直接顶爆 dram0_0_seg(D24A 实测溢出 8.8KB)。
//
// 取值方式:代码里照旧写 THEME_BG_COLOR 这些名字,它们是转发宏(见文件末尾)。
extern const Theme* g_theme_ptr;

// 默认主题(flash 里的常量),theme_load() 失败时指针指向它
const Theme& theme_defaults();
// 加载缓冲:theme_store 解析主题文件时直接解到这里
void theme_loaded_slot(Theme** out);
// 解析成功后把生效指针切到加载缓冲
void theme_use_loaded();

#define g_theme (*g_theme_ptr)

// 启动时确保主题可用。
// ★ 只在"当前没有可用主题"时落到默认值 —— **不会**覆盖已加载的主题。
//   调用方典型顺序:theme_load() 然后 dash_ui_init()(内部调本函数)。
//   早期版本在这里无条件重置,导致刚加载的主题被丢掉
//   (日志说"已加载",画面却是默认配色)。
void theme_reset_to_defaults();
// 无条件回到默认主题(显式"恢复默认"用)
void theme_force_defaults();
// 值域兜底 + 派生字段重算。两条路径共用（见 ui_theme.cpp）。
void theme_clamp(Theme& t);

// ---------------- 转发宏 ----------------
// 代码里仍写原来的名字,但值来自运行时主题。dash_ui_init() 之前
// 不要用这些宏（那时主题还没 load）。
#define THEME_BG_COLOR      (lv_color_hex(g_theme.bg_color))
#define THEME_FACE_SIZE     (g_theme.face_size)
#define FACE_BG_IDLE        (lv_color_hex(g_theme.face_bg_idle))
#define FACE_BG_REDLINE     (lv_color_hex(g_theme.face_bg_redline))
#define FACE_INK            (lv_color_hex(g_theme.face_ink))
#define FACE_EYE_L_X        (g_theme.eye_l_x)
#define FACE_EYE_R_X        (g_theme.eye_r_x)
#define FACE_EYE_Y          (g_theme.eye_y)
#define EYE_NORMAL_W        (g_theme.eye_normal_w)
#define EYE_NORMAL_H        (g_theme.eye_normal_h)
#define EYE_SURPRISE        (g_theme.eye_surprise)
#define EYE_NARROW_H        (g_theme.eye_narrow_h)
#define MOUTH_LINE_X        (g_theme.mouth_line_x)
#define MOUTH_LINE_Y        (g_theme.mouth_line_y)
#define MOUTH_LINE_W        (g_theme.mouth_line_w)
#define MOUTH_LINE_H        (g_theme.mouth_line_h)
#define MOUTH_O_X           (g_theme.mouth_o_x)
#define MOUTH_O_Y           (g_theme.mouth_o_y)
#define MOUTH_O_SIZE        (g_theme.mouth_o_size)
#define kCoolantMinC        (g_theme.coolant_min_c)
#define kCoolantMaxC        (g_theme.coolant_max_c)
#define kScreens            (g_theme.screens)
#define BOOT_FADE_MS        (g_theme.boot_fade_ms)
#define BOOT_SWEEP_START_MS (g_theme.boot_sweep_start_ms)
#define BOOT_STAGGER_MS     (g_theme.boot_stagger_ms)
#define BOOT_SWEEP_RISE_MS  (g_theme.boot_sweep_rise_ms)
#define BOOT_SWEEP_HOLD_MS  (g_theme.boot_sweep_hold_ms)
#define BOOT_SWEEP_FALL_MS  (g_theme.boot_sweep_fall_ms)
#define BOOT_FACE_START_MS  (g_theme.boot_face_start_ms)
#define BOOT_FACE_BLINK_MS  (g_theme.boot_face_blink_ms)
#define BOOT_TOTAL_MS       (g_theme.boot_total_ms)

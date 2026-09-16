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

// 一条弧的样式。
//
// ★ `reverse` —— 涨幅从**哪一端**开始涨(用户说的"镜像"):
//     0(默认):start 端固定,值从 start 往 end 涨。外圈转速弧就是这样:
//              起点 7:30,顺时针经过左、上、右,涨到 4:30。
//     1:end 端固定,值从 end 往回涨 —— 几何上等于把这条弧**水平镜像**一遍。
//   为什么需要它:水温弧是"下方半圆"(0°→180°,开口朝上),而 LVGL 的弧只能从
//   start 顺时针画到 end,所以只会从**右边**(3 点钟)开始亮。
//   水温表按惯例该从**左边**开始涨,所以给它 reverse=1:
//   点亮区从 180°(9 点钟)往 0°(3 点钟)长,读起来就是"从左往右涨"。
struct ArcStyle {
  ArcKind  kind;
  int32_t  start_deg;  // 0°=3点钟,顺时针;见 README 的角度约定
  int32_t  end_deg;
  int32_t  radius;     // 弧的**外沿**半径(width 往里长;480 基准,渲染时乘 theme_scale())
  int32_t  width;      // 弧线宽(480 基准)
  lv_color_t track_color;  // 未点亮轨道
  uint8_t  track_opa;      // 0..255
  lv_color_t value_color;  // 已点亮部分
  uint8_t  reverse;        // 1 = 从 end 端起涨(镜像);见上面的说明
};

struct ScreenTheme {
  ArcStyle arcs[kMaxArcs];
  uint8_t  arc_count;
  uint8_t  show_face;
};

// ============================================================
// 数字读数(转速/速度/水温)的样式与位置
//
// 为什么单独一个结构体而不是塞进 ScreenTheme:
//   两个表的读数**布局一样**(都在正上方),只是数据源和单位不同;
//   水温只有转速表有。放全局一份比每屏复制一遍更不容易写歪。
//
// 位置都是 **480 基准**坐标,乘 theme_scale() 后使用;圆心在 (240,240)。
// 竖直排布(480 基准,按 `radius` = **外沿**的语义算 —— 见 ArcStyle):
//   外弧带(radius 205 / width 24):外沿 y=35、**内沿 y=59**
//   数字中心   y = digit_cy        ← 默认 72,48 号墨迹实测 y[55..88]
//   单位中心   y = unit_cy         ← 默认 107,18 号墨迹实测 y[104..117]
//   表情顶边   y=120  ← 再往下会被表情压到
//
// ★ 这几个数是**实测量出来的**(固件落帧读墨迹),不是从公式推的。
//   曾经这里按"带宽居中"的旧模型写着弧带 23..47 —— 那是错的(外沿语义下是 35..59)。
// ★ 48 号数字的墨迹顶(55)比弧带内沿(59)高 4 像素,也就是数字顶上轻轻蹭到弧带。
//   这是**刻意接受**的:读数标签创建在弧**之后**,压在弧上面,4 像素的蹭边看不出来;
//   而为了躲开它把数字往下挪 4 像素,单位就会贴到表情顶边(119 对 120),反而更糟。
//   test_readout_defaults_fit_gap 按这个口径钉着(允许 ≤6px 蹭边、绝不许压表情)。
// ★ 不要为了"更靠上"把 digit_cy 往前挪 —— 再往上就是明显的"数字骑在弧上"。
// ============================================================
struct ReadoutTheme {
  uint32_t digit_color;     // 大数字颜色(转速/速度共用)
  uint32_t unit_color;      // 单位文字颜色(km/h / rpm)
  uint32_t coolant_color;   // 水温数字颜色

  uint8_t  digit_font;      // 0 = 48 号,1 = 18 号(见 readout_font())
  uint8_t  unit_font;       // 0 = 48 号,1 = 18 号

  int16_t  digit_cy;        // 大数字中心 y(480 基准;两屏共用)
  int16_t  unit_cy;         // 单位中心 y
  int16_t  coolant_cy;      // 水温数字中心 y(左屏底部)

  uint8_t  show_units;      // 0 = 只显示数字不显示单位
  uint8_t  show_coolant;    // 0 = 不显示水温数字
};

// 字体选择:0 = 48 号(大),1 = 18 号(小)。
// 用编号而不是直接存指针,是为了让主题能存进 JSON(指针没法序列化)。
const lv_font_t* readout_font(uint8_t which);

// 存档:数字读数的转发宏(与其它主题字段同一套用法)
#define READOUT_DIGIT_COLOR   (g_theme.readout.digit_color)
#define READOUT_UNIT_COLOR    (g_theme.readout.unit_color)
#define READOUT_COOLANT_COLOR (g_theme.readout.coolant_color)
#define READOUT_DIGIT_FONT    readout_font(g_theme.readout.digit_font)
#define READOUT_UNIT_FONT     readout_font(g_theme.readout.unit_font)
#define READOUT_DIGIT_CY      (g_theme.readout.digit_cy)
#define READOUT_UNIT_CY       (g_theme.readout.unit_cy)
#define READOUT_COOLANT_CY    (g_theme.readout.coolant_cy)
#define READOUT_SHOW_UNITS    (g_theme.readout.show_units)
#define READOUT_SHOW_COOLANT  (g_theme.readout.show_coolant)

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

  // ------------------------------------------------------------
  // 数字读数(转速/速度数字 + 单位 + 水温)
  //
  // 布局按实车(法系:左=转速表,右=速度表)与使用习惯定死:
  //   · 转速/速度的**大数字**放表盘正上方(y = 弧带最高点 23 到表情顶边 120 之间)
  //   · 单位(km/h、rpm)紧跟数字下方
  //   · 水温数字放**转速表(左屏)底部**
  //   中央 240×240 留给表情图。
  //
  // 位置按 **480 基准**书写,渲染时乘 theme_scale()(与弧/表情同一套规矩)。
  // 这些值可调,但要注意上方可用高度只有约 97 像素 —— 字号加大就会压到弧或表情。
  // ------------------------------------------------------------
  ReadoutTheme readout;

  // 开机动画(ms):淡入 → 双屏错峰扫表 → 表情出现
  uint32_t boot_fade_ms;
  uint32_t boot_sweep_start_ms;
  uint32_t boot_stagger_ms;
  uint32_t boot_sweep_rise_ms;
  uint32_t boot_sweep_hold_ms;
  uint32_t boot_sweep_fall_ms;
  uint32_t boot_face_start_ms;
  // ★ 字段名保留了历史(JSON 键不能随便改,老主题文件要能继续读),
  //   但含义已变:眨眼状态删掉后,它是"表情出现后的一拍收尾"。
  //   总时长 = boot_face_start_ms + 这一拍(见 theme_clamp)。
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

  // 数字读数。位置见 ReadoutTheme 的说明:干净可用的是 47..120 那 73 像素。
  t.readout.digit_color   = 0xFFFFFF;   // 白
  t.readout.unit_color    = 0x9AA0A6;   // 灰(单位不该抢数字的注意力)
  t.readout.coolant_color = 0x7CFF6B;   // 与水温弧同色,一眼能对上
  t.readout.digit_font    = 0;          // 48 号
  t.readout.unit_font     = 1;          // 18 号
  t.readout.digit_cy      = 72;         // 48 号数字高约 50 → 占 47..97(紧贴弧带下沿)
  t.readout.unit_cy       = 107;        // 18 号高约 20 → 占 97..117(紧贴表情顶边)
  t.readout.coolant_cy    = 384;        // 表盘底部(弧带在那里是空的,居中放得下)
  t.readout.show_units    = 1;
  t.readout.show_coolant  = 1;

  t.boot_fade_ms        = 250;
  t.boot_sweep_start_ms = 250;
  t.boot_stagger_ms     = 100;
  t.boot_sweep_rise_ms  = 380;
  t.boot_sweep_hold_ms  = 140;
  t.boot_sweep_fall_ms  = 420;
  t.boot_face_start_ms  = 1000;
  t.boot_face_blink_ms  = 130;   // 表情出现后的一拍收尾(名字是历史遗留,见字段注释)
  t.boot_total_ms       = t.boot_face_start_ms + t.boot_face_blink_ms;

  // ★ 屏幕布局按**法系车**来:左 = 转速表,右 = 速度表。
  //   (标致 206 实车就是这样,别按"左车速右转速"的日德习惯改回去。)
  //   水温表在**转速表**上,所以水温弧属于左屏。

  // 左屏 = 转速表:转速弧 + 水温弧(水温在表盘下方)
  //
  // ★ 两条弧**开口方向刻意相反**(用户要求:同向看着怪):
  //     转速弧(外):135 → 405,**缺口在正下方**,拱在表盘上方
  //     水温弧(内):  0 → 180,**缺口在正上方**,兜在表盘下方
  //   于是"外圈拱上面、内圈兜下面",一眼能分清哪条是哪条。
  //
  //   水温弧为什么是 0→180(而不是更短的弧):LVGL 的弧只能从 start 顺时针画到
  //   end,而"缺口在正上方"= 覆盖区必须落在下半圆,唯一干净的写法就是
  //   [0°, 180°](3 点钟 → 6 点钟 → 9 点钟)。代价是它的**填充方向是从右往左**
  //   (右边先亮、绕到左边),不像转速表那样从左往右。屏幕上有数字读数,
  //   所以不靠弧的左右去区分冷热;真要 C 在左 H 在右,得把弧放到上半圆
  //   (缺口朝下),那样又和转速表同向了 —— 这两个只能选一个。
  ScreenTheme& L = t.screens[0];
  L.arc_count = 2; L.show_face = 1;
  L.arcs[0] = ArcStyle{ ArcKind::Rpm,     135, 405, 205, 24,
                        lv_color_hex(0x232323), 153, lv_color_hex(0xFF5C5C), 0 };
  // reverse=1:水温弧从**左端(9 点钟)**起涨 —— 与转速弧形成镜像关系
  L.arcs[1] = ArcStyle{ ArcKind::Coolant,   0, 180, 168, 10,
                        lv_color_hex(0x232323), 153, lv_color_hex(0x7CFF6B), 1 };

  // 右屏 = 速度表:车速弧
  ScreenTheme& R = t.screens[1];
  R.arc_count = 1; R.show_face = 1;
  R.arcs[0] = ArcStyle{ ArcKind::Speed,   135, 405, 205, 24,
                        lv_color_hex(0x232323), 153, lv_color_hex(0x39C5FF), 0 };
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

#include "dash_ui.h"
#include "dash_display.h"
#include "ui_theme.h"
#include "boot_anim.h"
#include "image_load.h"     // 图片资源(背景图 / 表情图)
#include "face_stages.h"    // 表情槽位 → 图片角色 / 缺图降级链
#include <lvgl.h>
#include <Arduino.h>
#include <math.h>
#include "dash_log.h"   // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

// 表情槽位下标 = (uint8_t)Face —— 两者必须一样长,否则数组会越界
static_assert((uint8_t)Face::Count == kFaceSlotCount,
              "Face 枚举与 kFaceSlotCount 不一致:改枚举要同步 face_stages.h");

// ============ 运行时对象 ============
struct ScreenUi {
  uint8_t idx = 0;                  // 这是第几屏(0=左/转速表,1=右/速度表);图片按屏取
  lv_obj_t* arcs[kMaxArcs];
  uint8_t arc_count = 0;
  float arc_cur[kMaxArcs];          // 弧当前值(缓动用),开机扫表后从这里平滑过渡
  lv_obj_t* face_bg = nullptr;
  lv_obj_t* eye_l = nullptr;
  lv_obj_t* eye_r = nullptr;
  lv_obj_t* mouth = nullptr;
  Face last_face = Face::Count;     // 首帧强制全量应用

  // 数字读数(转速/速度大数字 + 单位 + 水温)
  lv_obj_t* digit_lbl = nullptr;
  lv_obj_t* unit_lbl = nullptr;
  lv_obj_t* coolant_lbl = nullptr;       // 左屏副表:水温
  lv_obj_t* intake_lbl = nullptr;        // 右屏副表:进气温度
  bool unit_set = false;                 // 单位文本写过没有(见 readout_apply)
  ArcKind digit_kind = ArcKind::Speed;   // 大数字跟的是哪条弧(建屏时定)
  int32_t digit_val = INT32_MIN;         // 上次显示的值:不变就不 set_text
  int32_t coolant_val = INT32_MIN;
  int32_t intake_val = INT32_MIN;

  // ---- 指示灯槽位(2026-09-24)----
  // 每个槽 = 一个**容器**(占位图形的父对象) + 子图形。
  // 只存对象指针:亮/灭/闪烁全部靠 opa 与 HIDDEN 旗标表达,
  // **不重建对象**(重建会在 LVGL 里留下 invalid 记录,闪烁时 5 Hz 重建更糟)。
  lv_obj_t* lamp[kLampSlotCount] = {};
  uint8_t lamp_sub[kLampSlotCount] = {};        // 这个槽有几个子图形(建槽时定)
  uint8_t lamp_last_pulse[kLampSlotCount] = {}; // 上次的亮度:不变就一个字节都不碰
  bool lamp_last_alert[kLampSlotCount] = {};
  bool lamp_built = false;
};

static ScreenUi g_ui[2];
static lv_obj_t* g_screens[2];
static BootAnim g_boot;
static bool g_boot_done_printed = false;
static uint32_t last_ok_ms = 0;
static uint32_t last_tick_ms = 0;

// ============ 图片资源 ============
// lv_image_dsc_t 必须由我们持有 —— LVGL 会一直引用它(set_src 不复制)。
// 每屏一张背景 + 每屏 4 个状态的表情(两屏的状态集合不完全一样,见 face_stages.h)。
// 下标 = (uint8_t)Face(见 expression.h:枚举顺序就是槽位顺序)。
static lv_image_dsc_t g_bg_dsc[2];
static lv_image_dsc_t g_face_dsc[2][kFaceSlotCount];
static bool g_bg_ok[2] = {false, false};
static bool g_face_ok[2][kFaceSlotCount] = {};
static lv_obj_t* g_bg_img[2] = {nullptr, nullptr};
static lv_obj_t* g_face_img[2] = {nullptr, nullptr};
static int8_t g_face_slot[2] = {-1, -1};    // 当前正显示哪一张(-1 = 还没显示过图片)

// 槽位 → 角色。角色编号表在 face_stages.h(kFaceRoleId),
// 那里复述了 image_blob.h 的 ImageRole —— 由宿主机测试逐条比对,
// 所以"刷进去的表情左右颠倒"这种错不会悄悄发生。
// ★ 0 表示"这屏用不到这个状态"(左屏没有超速、右屏没有红区),
//   调用方必须把它当"没有图"处理,不能拿去 image_dsc_for_role()。
static ImageRole faceRole(uint8_t screen, uint8_t slot) {
  return (ImageRole)kFaceRoleId[screen][slot];
}

static bool faceSlotExists(uint8_t screen, uint8_t slot) {
  return kFaceRoleId[screen][slot] != 0;
}

// 该状态该用哪张图:**按降级链找第一张"这屏导入过"的**。
// 返回槽位下标;这张屏一张表情图都没有 → 返回 -1(交给程序化表情)。
//
// 为什么要降级链:一套 8 张图没人会一次凑齐。只导入常态一张时,
// 巡航/运动/红区都应该落到它,而不是"图片消失、变回占位圆脸"。
static int faceResolve(uint8_t screen, Face f) {
  const uint8_t slot = (uint8_t)f;
  if (slot >= kFaceSlotCount) return -1;
  const int8_t* chain = kFaceFallback[slot];
  for (uint8_t i = 0; i < 4; ++i) {
    const int8_t s = chain[i];
    if (s >= 0 && s < (int8_t)kFaceSlotCount && g_face_ok[screen][s]) return s;
  }
  // 兜底:链里一条都没有,有图就用 ——
  // 图片摆在那儿却去画占位表情,才是最差的结果。
  for (uint8_t s = 0; s < kFaceSlotCount; ++s) {
    if (g_face_ok[screen][s]) return s;
  }
  return -1;
}

// 主题尺寸换算:480 基准 → 实际分辨率(四舍五入,见 ui_theme.h 分辨率适配)
static int32_t ts(float v480) {
  return (int32_t)lroundf(v480 * theme_scale());
}

// 把一条弧的进度(0..1)落到 LVGL 上。
//
// ★ 只有这一个地方决定"动的是哪一端",别在别处再算一遍:
//     reverse=0:start 端固定,动 end(值从 start 往 end 涨)
//     reverse=1:end   端固定,动 start(值从 end 往回涨 —— 视觉上是镜像)
//   为什么需要后者:水温弧是"下方半圆"(开口朝上),LVGL 只能从 start 顺时针画到
//   end,所以默认只会从右边(3 点钟)开始亮;水温表该从左端(9 点钟)起涨。
static void arc_set_progress(lv_obj_t* arc, const ArcStyle& a, float t) {
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  const int32_t span = a.end_deg - a.start_deg;
  if (a.reverse) {
    lv_arc_set_end_angle(arc, a.end_deg);                       // 固定端
    lv_arc_set_start_angle(arc, a.end_deg - (int32_t)(t * span));
  } else {
    lv_arc_set_start_angle(arc, a.start_deg);                   // 固定端
    lv_arc_set_end_angle(arc, a.start_deg + (int32_t)(t * span));
  }
}

// ============ 屏幕与控件构建 ============
static lv_obj_t* make_screen(lv_display_t* disp) {
  lv_display_set_default(disp);
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_screen_load(scr);   // v9:新屏必须显式加载,否则显示的是建屏时的默认屏
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, THEME_BG_COLOR, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  return scr;
}

static void build_arcs(lv_obj_t* parent, const ScreenTheme& cfg, ScreenUi& ui) {
  for (uint8_t i = 0; i < cfg.arc_count && i < kMaxArcs; ++i) {
    const ArcStyle& a = cfg.arcs[i];
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_center(arc);
    lv_obj_set_size(arc, ts(a.radius * 2), ts(a.radius * 2));

    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_start_angle(arc, a.start_deg);
    lv_arc_set_bg_end_angle(arc, a.end_deg);
    arc_set_progress(arc, a, 0.0f);   // 初始 0 进度(动哪一端由 reverse 决定)

    lv_obj_set_style_arc_width(arc, ts(a.width), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ts(a.width), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, a.track_color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, a.track_opa, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, a.value_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    // ★ 两个 part 都要圆头:MAIN 是轨道、INDICATOR 是点亮段,**一条弧的两端分属这两个 part**
    //   (固定端那半由点亮段画、另一端的收尾由轨道画)。原来只设了 INDICATOR →
    //   弧首看着是圆的、弧尾是平头(2026-09-21 落帧实测:弧首墨迹外伸 3.6°/1.8°
    //   = 端帽半径 w/2 对应的角度,弧尾 0°)。注意 lv_obj_remove_style_all 连 LVGL
    //   主题给 indicator 的 arc_rounded 也一起删了,所以两条都只能显式写。
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    ui.arcs[i] = arc;
  }
  ui.arc_count = cfg.arc_count;
}

// 占位表情:圆脸 + 双眼 + 嘴的形状组合。
// 换真实角色图时整段替换为 lv_image + 图片数组(见 ui_theme.h 注释)。
static void build_face(lv_obj_t* parent, ScreenUi& ui) {
  lv_obj_t* bg = lv_obj_create(parent);
  lv_obj_remove_style_all(bg);
  // 关键:带子对象(眼睛/嘴)的容器默认 LV_OBJ_FLAG_SCROLLABLE,
  // v9 会给可滚动容器开离屏层做裁剪,层的合成在本驱动下会偏移(顶带白斑即此因)。
  lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(bg);
  lv_obj_set_size(bg, ts(THEME_FACE_SIZE), ts(THEME_FACE_SIZE));
  lv_obj_set_style_bg_color(bg, FACE_BG_IDLE, 0);
  lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bg, ts(THEME_FACE_SIZE / 2), 0);

  lv_obj_t* eye_l = lv_obj_create(bg);
  lv_obj_t* eye_r = lv_obj_create(bg);
  lv_obj_t* eyes[] = {eye_l, eye_r};
  for (lv_obj_t* e : eyes) {
    lv_obj_remove_style_all(e);
    lv_obj_remove_flag(e, LV_OBJ_FLAG_SCROLLABLE);   // 圆角+滚动容器会走离屏层
    lv_obj_set_style_bg_color(e, FACE_INK, 0);
    lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
  }
  lv_obj_set_pos(eye_l, ts(FACE_EYE_L_X), ts(FACE_EYE_Y));
  lv_obj_set_pos(eye_r, ts(FACE_EYE_R_X), ts(FACE_EYE_Y));

  lv_obj_t* mouth = lv_obj_create(bg);
  lv_obj_remove_style_all(mouth);
  lv_obj_remove_flag(mouth, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(mouth, FACE_INK, 0);

  ui.face_bg = bg;
  ui.eye_l = eye_l;
  ui.eye_r = eye_r;
  ui.mouth = mouth;
}

// 有图片表情时,只切图、不碰程序化形状。
// 返回 true 表示这次由图片接管了。
static bool face_apply_image(uint8_t screen, Face f) {
  if (g_face_img[screen] == nullptr) return false;
  const int slot = faceResolve(screen, f);
  if (slot < 0) return false;                      // 这屏一张表情图都没有
  if (slot != g_face_slot[screen]) {               // 同一张图不重复 set_src
    g_face_slot[screen] = (int8_t)slot;
    lv_image_set_src(g_face_img[screen], &g_face_dsc[screen][slot]);
  }
  return true;
}

static void face_apply(ScreenUi& ui, Face f) {
  if (f == ui.last_face) return;
  ui.last_face = f;

  // ★ 有图片表情时由图片接管,程序化形状保持隐藏。
  //   注意 early return 必须在 last_face 更新之后 —— 否则每次都会重复判定。
  if (face_apply_image(ui.idx, f)) return;

  // 程序化占位表情:5 个状态里它只能表达"眯眼/睁大眼/张嘴/红底"这几种差别
  // (导入了图片就用图片,这一段只在完全没刷表情图时露脸)。
  // 第 4 档(原来的"惊喜"、现在是"超速")用大圆眼 + O 形嘴,正好也是"报警"的样子。
  const bool alarmed = (f == Face::Overspeed);
  const bool narrow =
      (f == Face::Cruise || f == Face::Sport || f == Face::Redline);
  const bool alarm = (f == Face::Redline);

  lv_obj_set_style_bg_color(ui.face_bg, alarm ? FACE_BG_REDLINE : FACE_BG_IDLE, 0);

  lv_obj_remove_flag(ui.eye_l, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(ui.eye_r, LV_OBJ_FLAG_HIDDEN);
  const uint8_t w = alarmed ? EYE_SURPRISE : EYE_NORMAL_W;
  const uint8_t h = alarmed ? EYE_SURPRISE : (narrow ? EYE_NARROW_H : EYE_NORMAL_H);
  lv_obj_set_size(ui.eye_l, ts(w), ts(h));
  lv_obj_set_size(ui.eye_r, ts(w), ts(h));
  lv_obj_set_style_radius(ui.eye_l, ts(h / 2), 0);
  lv_obj_set_style_radius(ui.eye_r, ts(h / 2), 0);

  if (alarmed) {
    lv_obj_set_pos(ui.mouth, ts(MOUTH_O_X), ts(MOUTH_O_Y));
    lv_obj_set_size(ui.mouth, ts(MOUTH_O_SIZE), ts(MOUTH_O_SIZE));
    lv_obj_set_style_radius(ui.mouth, ts(MOUTH_O_SIZE / 2), 0);
    lv_obj_set_style_bg_opa(ui.mouth, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.mouth, ts(4), 0);
    lv_obj_set_style_border_color(ui.mouth, FACE_INK, 0);
  } else {
    lv_obj_set_pos(ui.mouth, ts(MOUTH_LINE_X), ts(MOUTH_LINE_Y));
    lv_obj_set_size(ui.mouth, ts(MOUTH_LINE_W), ts(MOUTH_LINE_H));
    lv_obj_set_style_radius(ui.mouth, ts(MOUTH_LINE_H / 2), 0);
    lv_obj_set_style_bg_opa(ui.mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.mouth, 0, 0);
  }
}

// ============ 数字读数(转速/速度大数字 + 单位 + 水温) ============
// 位置、颜色、字体全部来自主题(ReadoutTheme);这里只管"取哪一路数据、
// 排成什么文字"。格式化规则刻意写死在固件里而不放进主题 —— 改格式等于改代码,
// 塞进主题只会让主题文件变成半个程序。
//
// 大数字显示哪一路?—— **由弧决定**,不看屏幕序号:
//   取该屏第一条"不是水温"的弧。这样以后把水温弧挪屏、或加第三条弧,
//   读数都自动跟着走,不需要同步改这里。

static bool screen_has_kind(const ScreenTheme& cfg, ArcKind k) {
  for (uint8_t i = 0; i < cfg.arc_count && i < kMaxArcs; ++i) {
    if (cfg.arcs[i].kind == k) return true;
  }
  return false;
}

// 副表 = 不占大数字的"小表":水温(左屏)、进气温度(右屏)。
// 它们只驱动自己那条内圈弧 + 屏底部一个数字。
//
// ★ 为什么要有这个判定函数,而不是到处写 `!= ArcKind::Coolant`:
//   大数字的规则是"取该屏第一条**非副表**的弧"。加进气温度时如果只加
//   `!= Coolant`,那么一条 [Intake, Speed] 顺序的屏会让速度表的大数字
//   显示成进气温度 —— 不报错、只是读数变错,很难查。所以副表要有个统一定义。
static bool is_aux_kind(ArcKind k) {
  return k == ArcKind::Coolant || k == ArcKind::Intake;
}

static ArcKind primary_kind(const ScreenTheme& cfg) {
  for (uint8_t i = 0; i < cfg.arc_count && i < kMaxArcs; ++i) {
    if (!is_aux_kind(cfg.arcs[i].kind)) return cfg.arcs[i].kind;
  }
  return (cfg.arc_count > 0) ? cfg.arcs[0].kind : ArcKind::Speed;
}

static const char* unit_text(ArcKind k) {
  switch (k) {
    case ArcKind::Speed: return "km/h";
    case ArcKind::Rpm:   return "rpm";
    default:             return "";
  }
}

// 显示值。转速取到 10 位:OBD 的转速本身就在几十转上下抖,个位纯噪声。
// 副表(水温/进气温度)都取整到 1℃ —— 它们是慢变量,小数位是噪声。
static int32_t readout_value(ArcKind k, const ArcDashView& v) {
  switch (k) {
    case ArcKind::Speed:  return (int32_t)lroundf(v.speed_kmh);
    case ArcKind::Rpm:    return (int32_t)(lroundf(v.rpm / 10.0f) * 10.0f);
    case ArcKind::Intake: return (int32_t)lroundf(v.intake_c);
    default:              return (int32_t)lroundf(v.coolant_c);
  }
}

// 读数用标签:定宽 + 文字居中,所以文本从"8"变到"8000"也不会左右挪位。
static lv_obj_t* make_readout_label(lv_obj_t* parent, const lv_font_t* font,
                                    lv_color_t color, int32_t cy480) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_remove_style_all(l);          // 只要文字:清掉内边距,免得隐形边框压住弧
  lv_obj_set_width(l, LV_PCT(100));
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, ts(cy480 - 240));   // cy 按 480 基准给
  // ★ 必须显式清空:lv_label_create() 建出来的标签**默认文本是 "Text"**,
  //   不清的话开机扫表期间表盘上会明晃晃写着两个 "Text"(实测在预览帧里抓到)。
  lv_label_set_text(l, "");
  return l;
}

static void build_readout(lv_obj_t* parent, const ScreenTheme& cfg, ScreenUi& ui) {
  const ArcKind pk = primary_kind(cfg);
  ui.digit_kind = pk;

  ui.digit_lbl = make_readout_label(parent, READOUT_DIGIT_FONT,
                                    lv_color_hex(READOUT_DIGIT_COLOR), READOUT_DIGIT_CY);
  if (READOUT_SHOW_UNITS) {
    // ★ 单位文本**故意留到第一次 readout_apply 才写**(见下面的 unit_set):
    //   建屏时写上,开机扫表那一段就会孤零零挂着个 "rpm" —— 数字出场前
    //   先出来一个单位,看起来像残影。
    ui.unit_lbl = make_readout_label(parent, READOUT_UNIT_FONT,
                                     lv_color_hex(READOUT_UNIT_COLOR), READOUT_UNIT_CY);
  }

  // 副表数字:该屏真的有这条弧、主题也允许,才建。
  // 若这屏唯一那条弧就是副表(大数字已经在显示它了),就别在底下重复一遍。
  if (READOUT_SHOW_COOLANT && pk != ArcKind::Coolant &&
      screen_has_kind(cfg, ArcKind::Coolant)) {
    ui.coolant_lbl = make_readout_label(parent, READOUT_UNIT_FONT,
                                        lv_color_hex(READOUT_COOLANT_COLOR),
                                        READOUT_COOLANT_CY);
  }
  // 进气温度同上一套(右屏副表)。两条副表的位置字段是**分开的**
  // (coolant_cy / intake_cy),所以万一有人把两条内圈弧放到同一屏,
  // 也能各自挪开,不会叠在一起。
  if (READOUT_SHOW_INTAKE && pk != ArcKind::Intake &&
      screen_has_kind(cfg, ArcKind::Intake)) {
    ui.intake_lbl = make_readout_label(parent, READOUT_UNIT_FONT,
                                       lv_color_hex(READOUT_INTAKE_COLOR),
                                       READOUT_INTAKE_CY);
  }
  // ★ 标签一律以空文本创建:开机动画期间 dash_ui_render 会早退,
  //   于是"扫表时数字栏是空的",扫完第一帧才出现 —— 这正是想要的效果。
  //   刻意不做淡入:LVGL 给对象设 opa<255 会开离屏层,这个驱动上会错位(见 boot_apply)。
}

static void readout_apply(ScreenUi& ui, const ArcDashView& v) {
  // 单位:只取决于弧种类,所以只需要写一次 —— 但必须等到"读数该出现的时刻"
  // (开机扫表期间 dash_ui_render 会早退,所以这一句自然就推迟到扫表之后)。
  if (ui.unit_lbl && !ui.unit_set) {
    ui.unit_set = true;
    lv_label_set_text(ui.unit_lbl, unit_text(ui.digit_kind));
  }
  if (ui.digit_lbl) {
    const int32_t dv = readout_value(ui.digit_kind, v);
    if (dv != ui.digit_val) {          // 只有真的变了才碰 LVGL:读数每秒都在刷,
      ui.digit_val = dv;               // 无脑 set_text 会把 16 条 invalid 队列刷爆
      lv_label_set_text_fmt(ui.digit_lbl, "%d", (int)dv);
      // 文本长度变了 self size 就变,重 align 一次最稳(定宽 + 居中其实已够)
      lv_obj_align(ui.digit_lbl, LV_ALIGN_CENTER, 0, ts(READOUT_DIGIT_CY - 240));
    }
  }
  if (ui.coolant_lbl) {
    const int32_t cv = (int32_t)lroundf(v.coolant_c);
    if (cv != ui.coolant_val) {
      ui.coolant_val = cv;
      lv_label_set_text_fmt(ui.coolant_lbl, "%d\xC2\xB0""C", (int)cv);   // 88°C
      lv_obj_align(ui.coolant_lbl, LV_ALIGN_CENTER, 0, ts(READOUT_COOLANT_CY - 240));
    }
  }
  if (ui.intake_lbl) {
    const int32_t iv = (int32_t)lroundf(v.intake_c);
    if (iv != ui.intake_val) {
      ui.intake_val = iv;
      lv_label_set_text_fmt(ui.intake_lbl, "%d\xC2\xB0""C", (int)iv);    // 34°C
      lv_obj_align(ui.intake_lbl, LV_ALIGN_CENTER, 0, ts(READOUT_INTAKE_CY - 240));
    }
  }
}

// ============ 指示灯槽位（占位图形，2026-09-24）============
//
// ★★ 本轮**只画占位几何**，不画正式素材 —— 这是刻意的（让逻辑与美术解耦）：
//     · 几何/位置在 src/ui_model.h（kLampSize / kLampGap / kLampCy，480 基准，
//       随 theme_scale() 缩放 ⇒ 480 与 240 两档自动各自成立）；
//     · 亮/灭/闪烁/告警描边由 make_lamps() 算好（纯函数，native 有几何用例）；
//     · 这里**只负责把"亮不亮"变成像素**：一个槽一个容器 + 若干子图形。
//   真屏到了换正式素材时，要改的**只有本函数下面那几段图形构建**
//   （换成 image 或 LVGL 的矢量/自定义 draw），本文件其余部分、ui_model、
//   alerts、data_service 全都不用动。
//
// 图形用**最朴素的几何**（矩形/圆/旋转矩形），每个槽一眼能认出是什么：
//   左转 = 双层左尖括号(chevron)   右转 = 镜像
//   双闪 = 两个三角并排(报警符号)    近光 = 半圆 + 三条斜光线
//   仪表盘灯 = 实心圆(灯珠)         门   = 侧立的矩形门扇 + 门把手圆点
//
// ★ 为什么全部用 LVGL 对象而不是自绘：对象可以**只改 opa/旗标**地闪烁
//   （见 lamp_apply），而自绘每次都要 invalidate 一整块。
// ★ 不给这些对象设 opa < 255 的**父容器**：这个驱动上会给对象开离屏层
//   （见 boot_apply 那条踩坑记录）。所以亮度落在**每个子图形**上，
//   容器本身恒为不透明（它没有背景，只是坐标系）。

// 建一个"纯容器"：无样式、不可滚动、按槽位摆好。
static lv_obj_t* lamp_make_cell(lv_obj_t* parent, LampSlot slot) {
  lv_obj_t* cell = lv_obj_create(parent);
  lv_obj_remove_style_all(cell);
  // ★ 不设 SCROLLABLE 会走离屏层裁剪（与表情容器同一个坑，见 build_face）
  lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(cell, ts(kLampSize), ts(kLampSize));
  lv_obj_set_pos(cell, ts(lampLeft(slot)), ts(lampTop(slot)));
  lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);   // 默认灭：第一帧由 lamp_apply 决定
  return cell;
}

// 槽内的小矩形（三角形/光线/门扇都由它拼）
static lv_obj_t* lamp_make_bar(lv_obj_t* cell, lv_color_t c, int32_t x, int32_t y,
                              int32_t w, int32_t h, int32_t rot10) {
  lv_obj_t* o = lv_obj_create(cell);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(o, ts(w), ts(h));
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, ts(1), 0);   // 一点倒角,免得细条端点太尖
  lv_obj_set_pos(o, ts(x), ts(y));
  // ★ LVGL 的旋转是 **0.1 度**为单位的整数,且绕对象中心转
  lv_obj_set_style_transform_rotation(o, rot10, 0);
  return o;
}

static lv_obj_t* lamp_make_dot(lv_obj_t* cell, lv_color_t c, int32_t cx, int32_t cy,
                              int32_t d) {
  lv_obj_t* o = lamp_make_bar(cell, c, cx - d / 2, cy - d / 2, d, d, 0);
  lv_obj_set_style_radius(o, ts(d / 2), 0);   // 全圆角 = 圆
  return o;
}

// 双层 chevron：`dir` = +1 指右 / -1 指左。返回子图形个数。
// ★ 画法：两根细长条各转 ±θ，拼成一个"<"；两层错开就是双箭头。
//   为什么不用三角形：LVGL 没有现成的三角形图元，而"两根条拼一个尖角"
//   是纯矩形 + 旋转，行为在任何驱动上都一样（自绘路径要碰 draw 回调，
//   那条路在这个精简版 esp_lcd 上还没验过）。
//
// ★ 几何用**"尖角位置 + 臂长"**算出来，不靠试：条的中心 = 尖角 + (L/2)·(cosθ, ±sinθ)
//   （θ = 30° ⇒ 0.866L/2, 0.5L/2）。这样槽内怎么挪都只是改 `tip` 一个数。
static uint8_t lamp_build_chevron(lv_obj_t* cell, lv_color_t c, int32_t dir) {
  const int32_t L = 17;    // 条长
  const int32_t T = 5;     // 条厚
  const int32_t ang = 30;  // 与水平线的夹角(度)
  const int32_t dx = 7;    // (L/2)·cos30 ≈ 7.4 → 取 7
  const int32_t dy = 4;    // (L/2)·sin30 ≈ 4.25 → 取 4
  uint8_t n = 0;
  for (int32_t layer = 0; layer < 2; ++layer) {
    // 尖角的 x：左箭头从 7 起往右排两层；右箭头镜像。
    const int32_t tip_x = (dir < 0) ? (7 + layer * 9) : (33 - layer * 9);
    const int32_t tip_y = 20;
    // ★ 上臂转 -30°、下臂转 +30° —— **与 dir 无关**：一个 "<" 和一个 ">"
    //   用的是同一对角度，只是尖角的 x 镜像了（第一版在这里按 dir 又翻了一次，
    //   于是"右箭头"画出个"左箭头"，而且不报错）。
    const int32_t bx = (dir < 0 ? tip_x + dx : tip_x - dx) - L / 2;
    lamp_make_bar(cell, c, bx, tip_y - dy - T / 2, L, T, -ang * 10);
    lamp_make_bar(cell, c, bx, tip_y + dy - T / 2, L, T, +ang * 10);
    n += 2;
  }
  return n;
}

// 建一个槽的占位图形。返回子图形个数（0 = 这个槽没有图形，用例会拦）。
static uint8_t lamp_build_slot(lv_obj_t* cell, LampSlot slot, lv_color_t c) {
  switch (slot) {
    case LampSlot::LeftArrow:  return lamp_build_chevron(cell, c, -1);
    case LampSlot::RightArrow: return lamp_build_chevron(cell, c, +1);
    case LampSlot::Hazard: {
      // 两个三角并排(常见双闪符号):每个三角 = 两根斜条 + 一根横条
      uint8_t n = 0;
      for (int32_t k = 0; k < 2; ++k) {
        const int32_t cx = 11 + k * 18;
        lamp_make_bar(cell, c, cx - 8, 12, 4, 16, 20 * 10);
        lamp_make_bar(cell, c, cx - 8, 12, 4, 16, -20 * 10);
        lamp_make_bar(cell, c, cx - 9, 26, 18, 4, 0);
        n += 3;
      }
      return n;
    }
    case LampSlot::LowBeam: {
      // 圆 + 三条向下斜的光线(近光的通用符号)。
      // ★ 一开始想画"半圆 + 光线",但那要**按角设圆角**(lv_obj 的 radius
      //   只有整体/四角同值),而这个精简版驱动上没验过自绘路径 —— 于是
      //   改成"圆 + 光线":同样一眼可辨,而且只用矩形/圆两种图元。
      uint8_t n = 0;
      lamp_make_dot(cell, c, 13, 20, 16);
      n++;
      lamp_make_bar(cell, c, 22, 8, 13, 3, 35 * 10);
      lamp_make_bar(cell, c, 24, 18, 13, 3, 0);
      lamp_make_bar(cell, c, 22, 28, 13, 3, -35 * 10);
      n += 3;
      return n;
    }
    case LampSlot::PositionLamp:
      // 仪表盘灯 = 实心圆(灯珠)。不画光芒:它要能一眼区别于近光
      lamp_make_dot(cell, c, 20, 20, 18);
      return 1;
    case LampSlot::Door: {
      // 门扇(侧立矩形) + 门把手圆点
      lamp_make_bar(cell, c, 12, 8, 15, 24, 0);
      lamp_make_dot(cell, c, 23, 20, 5);
      return 2;
    }
    default:
      return 0;
  }
}

// 建灯条(六个槽)。★ 创建顺序在**背景图之后**、读数之前 ——
// 灯条压在背景图上、被读数压在下面(读数在底部只有副表数字,不会重叠)。
static void build_lamps(lv_obj_t* parent, ScreenUi& ui) {
  const lv_color_t c = lv_color_hex(THEME_LAMP_COLOR);
  for (uint8_t i = 0; i < kLampSlotCount; ++i) {
    lv_obj_t* cell = lamp_make_cell(parent, (LampSlot)i);
    ui.lamp[i] = cell;
    ui.lamp_sub[i] = lamp_build_slot(cell, (LampSlot)i, c);
    // 告警描边用的边框:**建好就设置、平时不显示**(改 opa 而不是改宽度,
    // 免得"开描边"那一下触发一次布局重算)
    lv_obj_set_style_border_width(cell, ts(2), 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(THEME_LAMP_ALERT_COLOR), 0);
    lv_obj_set_style_border_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(cell, ts(6), 0);
  }
  ui.lamp_built = true;
}

// 把 make_lamps() 的结果落到 LVGL 上。
// ★ 只在**亮度和告警位真的变了**的时候碰对象：转向灯 5 Hz 闪，
//   无脑每帧 set_opa 会把 invalid 队列刷爆（表情那段注释里踩过同一个坑）。
static void lamp_apply(ScreenUi& ui, const LampView& v) {
  if (!ui.lamp_built) return;
  for (uint8_t i = 0; i < kLampSlotCount; ++i) {
    const uint8_t pulse = v.pulse[i];
    const bool show = pulse > 0;
    if (pulse != ui.lamp_last_pulse[i]) {
      ui.lamp_last_pulse[i] = pulse;
      if (show) {
        lv_obj_remove_flag(ui.lamp[i], LV_OBJ_FLAG_HIDDEN);
        // 亮度落在**每个子图形**上(不是容器 —— 见 build_lamps 上的说明)
        const uint32_t kids = lv_obj_get_child_count(ui.lamp[i]);
        for (uint32_t k = 0; k < kids; ++k) {
          lv_obj_set_style_opa(lv_obj_get_child(ui.lamp[i], (int32_t)k), pulse, 0);
        }
      } else {
        lv_obj_add_flag(ui.lamp[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (v.alert[i] != ui.lamp_last_alert[i]) {
      ui.lamp_last_alert[i] = v.alert[i];
      lv_obj_set_style_border_opa(ui.lamp[i],
                                  v.alert[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
  }
}

// ============ 开机动画应用(20ms 档推进,见 dash_ui_tick 的节流) ============
static void boot_apply(uint32_t now) {
  for (uint8_t s = 0; s < 2; ++s) {
    // 注意:不给屏幕对象设 opa<255 的淡入 —— LVGL 会因此给整屏渲染开离屏层,
    // 而层缓冲从显示缓冲里切(ARGB8888),装不下整屏 → 下半屏内容回绕到顶部。
    // 想要淡入效果时用一个不透明黑底覆盖件反向淡出,别动屏幕本身的透明度。
    ScreenUi& ui = g_ui[s];
    const float p = g_boot.arcProgress(now, s);
    for (uint8_t i = 0; i < ui.arc_count; ++i) {
      ui.arc_cur[i] = p;   // 扫表直接跟随,结束后的数据缓动从这里起步
      arc_set_progress(ui.arcs[i], kScreens[s].arcs[i], p);
    }

    if (kScreens[s].show_face && (ui.face_bg || g_face_img[s])) {
      // 表情出现:阶段 0 透明、之后全显。只在阶段切换时 set 一次,
      // 避免每 tick 重复 set opa 触发无谓重绘(曾导致层合成异常)。
      // (眨眼状态已删除,所以这里不再有"闭眼/睁眼"来回切,只剩一次显形。)
      const uint8_t st = g_boot.faceStage(now);
      static uint8_t last_st[2] = {0xFF, 0xFF};
      if (st != last_st[s]) {
        last_st[s] = st;
        const lv_opa_t opa = (st == 0) ? LV_OPA_TRANSP : LV_OPA_COVER;
        if (ui.face_bg) lv_obj_set_style_opa(ui.face_bg, opa, 0);
        // ★ 有图片表情时淡入要作用在图片上 —— 否则"显形"这个开机动作
        //   在有图的情况下会完全消失(程序化那层被藏起来了)。
        if (g_face_img[s]) lv_obj_set_style_opa(g_face_img[s], opa, 0);
      }
      if (st >= 1) face_apply(ui, Face::Idle);
    }
  }
}

// ============ 渲染 ============
static float arc_progress(const ArcStyle& a, const ArcDashView& v) {
  float t = 0.0f;
  switch (a.kind) {
    case ArcKind::Speed:   t = v.speed_t; break;
    case ArcKind::Rpm:     t = v.rpm_t; break;
    case ArcKind::Coolant:
      t = (v.coolant_c - kCoolantMinC) / (kCoolantMaxC - kCoolantMinC);
      break;
    case ArcKind::Intake:
      t = (v.intake_c - kIntakeMinC) / (kIntakeMaxC - kIntakeMinC);
      break;
  }
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return t;
}

void dash_ui_init() {
  // 先落默认主题:保证任何情况下主题都是可用的。
  // main 的 setup() 会在调本函数之前尝试 theme_load() 覆盖它(读 flash 主题
  // 分区);这里兜底是为了"单独调 dash_ui_init 也不会拿到未初始化的主题"。
  theme_reset_to_defaults();

  lv_init();
  dash_display_init();

  lv_display_t* def = lv_display_get_default();
  g_screens[0] = make_screen(dash_display_left());
  g_screens[1] = make_screen(dash_display_right());
  lv_display_set_default(def);

  // 图片资源:先探测每个角色有没有图(没刷图片时全部 false,走降级路径)
  for (uint8_t s = 0; s < 2; ++s) {
    g_bg_ok[s] = image_dsc_for_role(ImageRole::Background, &g_bg_dsc[s]);
    for (uint8_t slot = 0; slot < kFaceSlotCount; ++slot) {
      // 这屏用不到的状态(角色号 0)不要去查图:查也查不到,但会把
      // "0 号角色"当成一个真实编号传下去,将来加角色时容易踩到。
      g_face_ok[s][slot] = faceSlotExists(s, slot) &&
                           image_dsc_for_role(faceRole(s, slot), &g_face_dsc[s][slot]);
    }
  }

  for (uint8_t s = 0; s < 2; ++s) {
    g_ui[s].idx = s;                 // 图片按屏取,index 必须先设
    // 图层顺序 = 创建顺序:背景图 → 弧线 → 表情。
    // 背景图必须是**第一个**子对象,这样它衬在弧线下面。
    if (g_bg_ok[s]) {
      g_bg_img[s] = lv_image_create(g_screens[s]);
      lv_image_set_src(g_bg_img[s], &g_bg_dsc[s]);
      lv_obj_center(g_bg_img[s]);
    }
    build_arcs(g_screens[s], kScreens[s], g_ui[s]);
    if (kScreens[s].show_face) build_face(g_screens[s], g_ui[s]);
  }

  // 用图片表情替换(或隐藏)程序化表情。
  // ★ 降级:没有表情图时**保留程序化形状表情** —— 这条路径是刻意留的,
  //   与"没有主题就用默认主题"是同一个原则:资源缺失不能让界面空掉。
  for (uint8_t s = 0; s < 2; ++s) {
    bool any = false;
    for (uint8_t i = 0; i < kFaceSlotCount; ++i) any = any || g_face_ok[s][i];
    if (!any) continue;

    if (g_face_img[s] == nullptr) {
      g_face_img[s] = lv_image_create(g_screens[s]);
      lv_obj_center(g_face_img[s]);
    }
    // 有图就把程序化表情藏起来(不能删 —— face_apply 还会去访问那几个对象)
    if (g_ui[s].face_bg) lv_obj_add_flag(g_ui[s].face_bg, LV_OBJ_FLAG_HIDDEN);
    // 先摆"常态该用的那张"(可能降级到别的槽),后续 face_apply 按状态切换
    const int slot = faceResolve(s, Face::Idle);
    g_face_slot[s] = (int8_t)slot;
    lv_image_set_src(g_face_img[s], &g_face_dsc[s][slot]);
  }

  // 指示灯槽位(占位图形):建在**读数之前** —— 于是副表数字(水温/进气)
  // 压在灯条上面；两者在几何上不重叠(灯条 y=395..435、副表墨迹到 y≈396),
  // 所以图层顺序在这里只是"万一"的保险(见 ui_model.h 的 kLamp* 说明)。
  for (uint8_t s = 0; s < 2; ++s) {
    build_lamps(g_screens[s], g_ui[s]);
  }

  // 数字读数最后建:创建顺序就是图层顺序,读数要压在弧和表情之上。
  for (uint8_t s = 0; s < 2; ++s) {
    build_readout(g_screens[s], kScreens[s], g_ui[s]);
  }

  g_boot.start(millis());
  dash_logf("206 dash boot\n");
}

void dash_ui_tick(uint32_t now_ms) {
  if (last_tick_ms != 0) lv_tick_inc(now_ms - last_tick_ms);
  last_tick_ms = now_ms;

  if (g_boot.active(now_ms)) {
    // 开机动画按 20ms(50Hz)档推进:扫表角度每档才 invalidate 一次。
    // 若按主循环频率(≈1kHz)每圈都 set 角度,LvGL 的 16 条 invalid 队列会被
    // 刷爆并 join 成全屏区域,走 tile 渲染路径,预览缓冲里出现错位残影。
    static uint32_t last_boot_ms = 0;
    if (now_ms - last_boot_ms >= 20) {
      last_boot_ms = now_ms;
      boot_apply(now_ms);
    }
  } else if (!g_boot_done_printed) {
    g_boot_done_printed = true;
    dash_logf("boot anim done\n");
  }

  lv_timer_handler();
}

void dash_ui_render(const ArcDashView& v, const LampView& lamps, uint32_t now) {
  if (now - last_ok_ms >= 1000) {
    last_ok_ms = now;
    // face= 打的是**左/右两个**:两屏表情各看各的表,只打一个就分不清
    // 是"转速档没生效"还是"车速档没生效"。
    dash_logf("206 dash ok  spd=%3.0f%% rpm=%3.0f%% coolant=%.0fC face=%s/%s\n",
                  v.speed_t * 100.0f, v.rpm_t * 100.0f, v.coolant_c,
                  face_name(v.face_left), face_name(v.face_right));
  }

  // ★ 灯条**在开机动画之前**应用:开机扫表期间也要能看见灯
  //   (打灯/开门是随时发生的,而开机动画只在前 1.1 秒)。它不参与弧的缓动。
  for (uint8_t s = 0; s < 2; ++s) lamp_apply(g_ui[s], lamps);

  if (g_boot.active(now)) return;   // 开机期间由 boot_apply 接管

  // 弧缓动:指数趋近,按实际经过时间算,渲染频率变化不影响手感
  static uint32_t last_render_ms = 0;
  float k;
  if (last_render_ms == 0) {
    k = 1.0f;   // 首帧直接到位
  } else {
    uint32_t dt = now - last_render_ms;
    if (dt > 500) dt = 500;   // 主循环卡顿后不跳变
    k = 1.0f - expf(-kArcSmoothPerSec * (float)dt * 0.001f);
  }
  last_render_ms = now;

  for (uint8_t s = 0; s < 2; ++s) {
    ScreenUi& ui = g_ui[s];
    for (uint8_t i = 0; i < ui.arc_count; ++i) {
      const ArcStyle& a = kScreens[s].arcs[i];
      const float target = arc_progress(a, v);
      ui.arc_cur[i] += (target - ui.arc_cur[i]) * k;
      arc_set_progress(ui.arcs[i], a, ui.arc_cur[i]);
    }
    if (kScreens[s].show_face && ui.face_bg) {
      // 表情的显隐/形变只在 face_apply 里按状态变化时改一次,这里不重复 set。
      // ★ 按屏取:**左屏用转速表的表情,右屏用速度表的表情**。
      const Face f = (s == 0) ? v.face_left : v.face_right;
      face_apply(ui, f);
    }
    readout_apply(ui, v);
  }
}

#include "dash_ui.h"
#include "dash_display.h"
#include "ui_theme.h"
#include "boot_anim.h"
#include "image_load.h"     // 图片资源(背景图 / 表情图)
#include "face_stages.h"    // 表情槽位 → 图片角色 / 缺图降级链
#include "system_status.h"  // 数据不可信提示 + 诊断页（纯逻辑住在 lib/dashcore/）
// ★ 开机时的角色标签（2026-09-25）要读 `LINK_ROLE` —— 那是"这块板是谁"的**唯一**出处
//   （编译期唯一权威，§5）。本文件不新增任何角色判据，只是把它印到屏上。
#include "link_role.h"
// ★★ 角色 ↔ 屏幕的映射（2026-09-25）：**"哪块板显示哪一屏"的唯一出处**在这个头里
//   （`lib/dashcore/dash_role_layout.h`，宿主机可测）。本文件里凡是"**这套表盘/这套表情/
//   这套图片角色**该给第 s 屏用哪一个下标"的地方，一律走它的两个映射函数 ——
//   本文件**不再**自己写 `s == 0 ? 左 : 右` 那种判据（两条判据迟早会分叉，
//   而分叉的症状是"转速表的弧配速度表的表情"——不报错、只是看着不对）。
#include "dash_role_layout.h"
#include <lvgl.h>
#include <Arduino.h>
#include <math.h>
#include "dash_log.h"   // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

// 表情槽位下标 = (uint8_t)Face —— 两者必须一样长,否则数组会越界
static_assert((uint8_t)Face::Count == kFaceSlotCount,
              "Face 枚举与 kFaceSlotCount 不一致:改枚举要同步 face_stages.h");

// ============ 运行时对象 ============
struct ScreenUi {
  // 这一屏的**屏号**（0 = 先建、1 = 末建/压在上面）。它是 LVGL 对象数组
  //   （`g_screens[]` / `g_face_img[]` / `g_bg_img[]`）与"哪一屏看得见"的键，
  //   **不是**表号 —— 表号是下面那个 `face_group`。
  uint8_t screen = 0;
  // ★★ 这一屏的**表情分组下标**（= 这一屏那块表的编号，见 `dash_role_layout.h` 第五节）。
  //   它同时是：主题表盘下标（`themeIndexForScreen`）、`kFaceRoleId` 的组号、
  //   以及 `g_face_ok/g_face_dsc` 的**第一维**。
  //   ★ 历史教训（2026-09-26）：这个字段以前叫 `idx` 并存的是**屏号**，
  //     而它的两个读者（表盘那一路 / 图片那一路）要的却是**表号** ⇒
  //     从板上 `s` 与 `ti` 对调，脸就按**车速**那 5 张图去挑了，
  //     车主看到的正是"表情跟转速没关系、随机变"。
  //   ⇒ 名字改成 `face_group`（说清它是"分组"不是"屏号"），并且**只由**
  //     `dashlayout::faceGaugeIndexForScreen(s)` 赋值。
  uint8_t face_group = 0;
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

  // ---- 数据不可信角标（2026-09-24）----
  // ★ 形态：**表盘右缘一枚小图标文字**（不是红屏、不是每秒闪）。
  //   它只在"数据不可信"期间出现，数据恢复后自动消失（去抖在
  //   lib/dashcore/system_status.h 里，屏这一层不做任何判断）。
  lv_obj_t* trust_badge = nullptr;      // 容器（默认 HIDDEN）
  lv_obj_t* trust_text = nullptr;       // 图标文字（"SIM" / "VAN?"）
  uint8_t   trust_last = 0xFF;          // 上次画的那一条理由（0xFF = 还没画过）

  // ---- 诊断页（2026-09-24）----
  // ★ 平时**完全不显示**：整个容器一建好就 HIDDEN，只有按 K（真机是长按/组合键）
  //   才显出来。本页读的是**本地现有信息**，零新数据源。
  lv_obj_t* diag_box = nullptr;
  lv_obj_t* diag_title = nullptr;
  lv_obj_t* diag_body = nullptr;
  bool      diag_built = false;

  // ---- ★★ 开机时的角色标签（2026-09-25）----
  // ★ 为什么要有它：两块 2.8C **外观完全一样**，而角色是**编译期**定死的
  //   （`LINK_ROLE`，§5）⇒ 手里这块板是哪一角色，只有刷进去的那份固件知道。
  //   开机动画那一秒半里在屏上打一行大字，就是让人**一眼**分清主/从。
  // ★ 它**只活到开机动画结束**（窗口一过就 HIDDEN）—— 表盘要保持干净，
  //   常驻就是 bug（车主的原话）。
  lv_obj_t* role_lbl = nullptr;
  bool      role_visible = false;       // 当前该不该显示（只在**变了**的时候碰 LVGL）
};

static ScreenUi g_ui[2];
static lv_obj_t* g_screens[2];
static BootAnim g_boot;
static bool g_boot_done_printed = false;
static uint32_t last_ok_ms = 0;
static uint32_t last_tick_ms = 0;

// ★★ 2026-09-26：`206 dash ok` 那一行的**节流闸门**。
//   为什么是 2 秒（而不是 1 秒或 5 秒）：见 `lib/dashcore/dash_log.h` 文件头那段
//   "日志绝不许阻塞主循环" —— 节流是为了**少写**，不是为了让读数变糊。
//   选 2 s 的三个具体理由：
//     · 它**整除** 5 秒那个摘要窗口（`kLoopStallReportMs`）⇒ 每个窗口里的样点
//       个数是确定的（2~3 个），不会忽多忽少；
//     · 它正好是"人看屏 + 看串口"能对上的尺度（屏上换档 → 1~2 秒内出现在串口）；
//     · 5 秒那一档就太糊了：这一行还兼着"`role=`/`last=` 到底生效没"的判据。
//   ★ 这一行属于"周期性遥测"，**可以**节流；守护/蜂鸣器/告警/诊断页那些
//     "变化那一刻才有意义"的行**一律不许**挂闸门（见 dash_log.h 的 `RateGate`）。
static dashlogring::RateGate g_ok_gate(2000u);

// ============ 数据不可信角标 / 诊断页（2026-09-24）============
//
// ★ 几何（**480 基准**，与弧/读数/灯条同一套 ts() 缩放 ⇒ 240 档自动成立）。
//   角标压**表盘右缘**那条留白：y = 205..241 那一带在 480 圆屏上
//   x=340 处仍在可视圆里（该行圆的右边界 ≈ x=452），而这一块
//   内容上与别的东西都不撞：
//     · 表情方框 200×200 居中 ⇒ x=140..340、y=140..340（角标在它右侧）
//     · 主弧带 181..205（按外沿半径 205、带宽 24 算）⇒ y=205 正好在带外
//     · 单位文字 cy=107、读数 cy=72、副表 cy=384 ⇒ 都在别处
//   ★ 为什么不放在"正中下方那块空地"：那里要留给副表数字（水温/进气），
//     而且中枢位置一有东西就"刺眼"—— 产品要求是**明确但不刺眼**。
static const int32_t kTrustBadgeX  = 340;
static const int32_t kTrustBadgeCY = 223;
static const int32_t kTrustBadgeW  = 116;
static const int32_t kTrustBadgeH  = 36;

// 角标**边框**颜色与底色按状态定：
//   · "来源/链路回退到 Sim"  → 灰蓝（信息级，不是故障）
//   · "VAN 断流 / 数据冻结"  → 琥珀（要人看一眼，但不刺眼）
// ★ 刻意**不用红色**：红屏/红角标是"故障"的语气，而"数据不可信"是
//   "这块表现在在演"，两者不是一回事（产品要求：明确但不刺眼）。
static uint32_t trustBadgeColor(DataTrustReason r) {
  switch (r) {
    case DataTrustReason::kVanStale:
    case DataTrustReason::kDataFrozen:   return 0xFFB020;   // 琥珀（与灯条同色系）
    default:                             return 0x7FA8C8;   // 灰蓝
  }
}

// 角标上的**短标记**。纯 ASCII（图标字面就三个字母，不画图形素材）：
//   SIM   = 这一格现在跑的是假数据（来源回退 Sim）
//   VAN?  = VAN 断了（"?"= 值不再有来源）
//   STALE = 帧还在来但值冻住了
//   LINK  = 双板链路退到 Sim（仅从板）
static const char* trustBadgeText(DataTrustReason r) {
  switch (r) {
    case DataTrustReason::kDataFallback: return "SIM";
    case DataTrustReason::kVanStale:     return "VAN?";
    case DataTrustReason::kDataFrozen:   return "STALE";
    case DataTrustReason::kLinkFallback: return "LINK";
    default:                             return "";
  }
}

// 诊断页的开/关与翻页状态。**只有这一个状态机**（不在别处再记一份）：
//   `g_diag_open` = 显不显示；`g_diag_page` = 第几页。
// ★ 设备端与 pcpreview 共用（main 只负责把按键翻译成这两个调用）。
static bool    g_diag_open = false;
static DiagPage g_diag_page = DiagPage::Sys;
// 上一次画进标签的诊断页文本（**逐字节比对**：不变就一个字节都不碰 LVGL）。
// 2048 是 16 行 × 最多 40 列 + 换行，够；它是一份静态缓冲，不进栈。
static char    g_diag_text[2048] = {0};
static char    g_diag_title[24] = {0};
// 几何自证那一行只打一次（见 dash_ui_diag_toggle）。
static bool    g_diag_geom_logged = false;

// 给"几何自证"那一行取字体的可读标记。
// ★★ 为什么**不能**打 `lv_font_t::name` / `::size`（本文件第一版就是这么写的，
//   在 pcpreview 上当场编不过：`no member named 'name' in '_lv_font_t'`）：
//   LVGL 9 把 `lv_font_t` 的实例字段留在了**私有头**里（include/ 那份公开头
//   只有回调与 `dsc`）⇒ 公开 API 里**拿不到**字体名。能拿到的只有这两个：
//     · `lv_font_get_line_height()` —— 公开函数，行高就是字号的可比数值；
//     · 指针相等比较 —— 本项目用到的字体是**编译期已知的那几个**
//       （`readout_font()` 只返回 Montserrat 10/18/24/48）。
//   这一行是给"以后页面上什么都没有"查尺寸用的，**不是**给人看字号设计；
//   所以"哪一档 + 行高"就够，不要去猜字体名。
static const char* diagFontTag(const lv_font_t* f) {
  if (!f) return "null";
  if (f == &lv_font_montserrat_10) return "m10";
  if (f == &lv_font_montserrat_18) return "m18";
  if (f == &lv_font_montserrat_24) return "m24";
  if (f == &lv_font_montserrat_48) return "m48";
  return "?";
}

// ============ 图片资源 ============
// lv_image_dsc_t 必须由我们持有 —— LVGL 会一直引用它(set_src 不复制)。
// 每屏一张背景 + 每屏 5 个状态的表情(两屏的状态集合不完全一样,见 face_stages.h)。
//
// ★★ 第一维是**表情分组**（= 表号，0=左/转速、1=右/车速），**不是屏号** ——
//   取值一律走 `faceGroupFor(s)`（= `dashlayout::faceGaugeIndexForScreen(s)`）。
//   从板上屏号与表号正好对调，混用就是车主报的"表情跟转速无关、随机变"。
//   第二维 = (uint8_t)Face(见 expression.h:枚举顺序就是槽位顺序)。
static lv_image_dsc_t g_bg_dsc[2];
static lv_image_dsc_t g_face_dsc[2][kFaceSlotCount];
static bool g_bg_ok[2] = {false, false};
static bool g_face_ok[2][kFaceSlotCount] = {};
static lv_obj_t* g_bg_img[2] = {nullptr, nullptr};
// 表情图片对象是**按屏**建的（每屏一个 LVGL 对象），所以这个数组的第一维是屏号。
static lv_obj_t* g_face_img[2] = {nullptr, nullptr};
static int8_t g_face_slot[2] = {-1, -1};    // 当前正显示哪一张(-1 = 还没显示过图片)

// 槽位 → 角色。角色编号表在 face_stages.h(kFaceRoleId),
// 那里复述了 image_blob.h 的 ImageRole —— 由宿主机测试逐条比对,
// 所以"刷进去的表情左右颠倒"这种错不会悄悄发生。
// ★ 0 表示"这屏用不到这个状态"(左屏没有超速、右屏没有红区),
//   调用方必须把它当"没有图"处理,不能拿去 image_dsc_for_role()。
//
// ★★ 2026-09-26（本单 B 的第二次修正）：下面这三个函数的第一个参数
//   **是"表号"（组号），不是"屏号"** —— 取值只有一个来源：
//   `dashlayout::faceGaugeIndexForScreen(s)`。名字里带 `g` 就是为了让
//   "屏号"与"组号"在调用点上一眼分得开（从板上这两个数正好对调，
//   混用的症状是"表情跟着另一块表的数据变"，不报错、只是看着不对）。
static ImageRole faceRole(uint8_t group, uint8_t slot) {
  return (ImageRole)kFaceRoleId[group][slot];
}

static bool faceSlotExists(uint8_t group, uint8_t slot) {
  return kFaceRoleId[group][slot] != 0;
}

// 该状态该用哪张图:**按降级链找第一张"这组导入过"的**。
// 返回槽位下标;这一组一张表情图都没有 → 返回 -1(交给程序化表情)。
//
// 为什么要降级链:一套 8 张图没人会一次凑齐。只导入常态一张时,
// 巡航/运动/红区都应该落到它,而不是"图片消失、变回占位圆脸"。
static int faceResolve(uint8_t group, Face f) {
  const uint8_t slot = (uint8_t)f;
  if (slot >= kFaceSlotCount) return -1;
  const int8_t* chain = kFaceFallback[slot];
  for (uint8_t i = 0; i < 4; ++i) {
    const int8_t s = chain[i];
    if (s >= 0 && s < (int8_t)kFaceSlotCount && g_face_ok[group][s]) return s;
  }
  // 兜底:链里一条都没有,有图就用 ——
  // 图片摆在那儿却去画占位表情,才是最差的结果。
  for (uint8_t s = 0; s < kFaceSlotCount; ++s) {
    if (g_face_ok[group][s]) return s;
  }
  return -1;
}

// ★★ 本单 B 的**唯一出口**：`g_face_ok` / `g_face_dsc` 的第一维就是这里给的下标。
//   `faceRole()` / `faceSlotExists()` / `faceResolve()` 三个都只认它。
//   ⇒ "表盘 / 表情槽位 / 表情图片"三条路共用 `dash_role_layout.h` 那一个式子。
static uint8_t faceGroupFor(uint8_t screen) {
  return dashlayout::faceGaugeIndexForScreen(screen);
}

// 背景图两屏共用一张（`image_blob.h`：`ImageRole::Background` 只有 1 号），
// 所以它**不按分组取** —— 分组是"表情"的事，背景没有左右之分。
static const ImageRole kBackgroundRole = ImageRole::Background;

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
//
// ★★ 参数是"这一屏" + "这次要显示哪张脸（哪条轴的表情）"，**不是**分组下标：
//   分组在函数里由 `faceGroupFor(screen)` 算 —— 那张脸属于哪块表，
//   就取哪块表的图片组。**一个式子，一处出口**。
static bool face_apply_image(uint8_t screen, Face f) {
  if (g_face_img[screen] == nullptr) return false;
  const uint8_t group = faceGroupFor(screen);
  const int slot = faceResolve(group, f);
  if (slot < 0) return false;                      // 这组一张表情图都没有
  if (slot != g_face_slot[screen]) {               // 同一张图不重复 set_src
    g_face_slot[screen] = (int8_t)slot;
    lv_image_set_src(g_face_img[screen], &g_face_dsc[group][slot]);
  }
  return true;
}

static void face_apply(ScreenUi& ui, Face f) {
  if (f == ui.last_face) return;
  ui.last_face = f;

  // ★ 有图片表情时由图片接管,程序化形状保持隐藏。
  //   注意 early return 必须在 last_face 更新之后 —— 否则每次都会重复判定。
  //   ★ `ui.face_group` 在这里**没有**被读：分组由 `face_apply_image()` 内部
  //     按"这一屏是哪块表"算（见那里的说明）。ui.face_group 只是给建屏那一段
  //     与日志用的同一份缓存，避免两处各算一次。
  if (face_apply_image(ui.screen, f)) return;

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

// 读数用标签:**内容宽度** + 对象居中 ⇒ 文本从"8"变到"8000"也不会左右挪位，
// 而"脏"的范围就是**文字自己的外框**（见下面那段"为什么不再强制整屏宽"）。
static lv_obj_t* make_readout_label(lv_obj_t* parent, const lv_font_t* font,
                                    lv_color_t color, int32_t cy480) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_remove_style_all(l);          // 只要文字:清掉内边距,免得隐形边框压住弧
  // ★★ 2026-09-24 第七轮：**不再** `lv_obj_set_width(l, LV_PCT(100))`。
  //
  //   旧写法把标签设成**整屏宽 480**、靠 `LV_TEXT_ALIGN_CENTER` 把文字摆在中间；
  //   而 LVGL 的 `lv_label_set_text*()` 失效的是**整个标签对象** ⇒ 数字每变一次
  //   就脏一条 **480×52** 的带（与单位标签并成 **480×78 = 一屏的 16.2%**），
  //   UI 5 拍/秒 ⇒ 每秒脏掉 **~86% 的一屏** —— 屏上表现就是车主说的
  //   "数字一跳，整块表像被刷了一刀"（每个刷新重画一条**横跨整屏**的读数带）。
  //
  //   改成"内容宽"之后：标签的框 = 文字自己的外框（大数字约 115×52、单位约 34×21），
  //   失效范围跟着缩到那一小块 —— 落帧实测（pcpreview 2.8C 档，静画）：
  //       单次最大脏矩形  480×78(16.2%) → **115×78(3.8%)**
  //       每秒脏面积      82.7k~98.1k px² → **25.8k~27.4k px²**（两屏合计，=单屏 11~12%）
  //   而 flash **一个字节都不涨**（这是**删**一行，没有新增任何数据/字体）。
  //   ★ 与"读数位置"无关：`lv_obj_align(..., LV_ALIGN_CENTER, 0, dy)` 居中的是
  //     **对象**，框变窄之后文字仍然在正中间（落帧实测墨迹 x/y 区间一个像素没变）。
  //   ★ 也没有引入新字体/图集/渲染路径 —— 仍旧是 LVGL 的 `lv_label`（数字串整体重画，
  //     只是重画的范围小了一个量级）。
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

// ============ 数据不可信角标 + 诊断页（2026-09-24）============
//
// 这一段**只负责画**：判据、去抖、限速、诊断页的字段映射全在
// `lib/dashcore/system_status.h`（纯逻辑，native 用例逐条钉住）。
// 于是"预览里看得见的提示"与"车上跳出来的提示"是同一段代码算的。
//
// ★ 建角标：一个容器 + 一个标签。
//   为什么不用「一个标签 + 边框」：这个驱动上给对象设 border 会走一遍
//   layout 重算（灯条那一段踩过），而角标是 5 Hz 一拍的显隐 ⇒ 容器自带
//   描边、标签只落文字，改的只有 opa/颜色/文本，不动尺寸。
static void build_trust_badge(lv_obj_t* parent, ScreenUi& ui) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(box, ts(kTrustBadgeW), ts(kTrustBadgeH));
  lv_obj_set_pos(box, ts(kTrustBadgeX), ts(kTrustBadgeCY - kTrustBadgeH / 2));
  lv_obj_set_style_bg_color(box, lv_color_hex(0x101010), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_70, 0);
  lv_obj_set_style_border_width(box, ts(2), 0);
  lv_obj_set_style_border_color(box, lv_color_hex(0x7FA8C8), 0);
  lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, ts(6), 0);
  lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);    // 默认不显示：第一帧由 trust_apply 决定

  lv_obj_t* t = lv_label_create(box);
  lv_obj_remove_style_all(t);
  lv_obj_set_style_text_font(t, READOUT_UNIT_FONT, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(t);
  lv_label_set_text(t, "");                    // ★ 必须显式清空（lv_label 默认是 "Text"）

  ui.trust_badge = box;
  ui.trust_text = t;
  ui.trust_last = 0xFF;
}

// ============================================================
// ★★ 开机时的**角色标签**（2026-09-25 新增）—— "这两块一模一样的板，我手里是谁？"
// ============================================================
// 车主的需求原话：「你烧的固件不能带标签吗，主片和副片的标签做区分。」
//
// 为什么**必须**是屏上的一行字（而不是"看串口/看诊断页"）：
//   · 两块 2.8C **外观完全一样**（同型号、同屏、同壳），插上电之后屏上显示的东西
//     也几乎一样 ⇒ 光看外观分不出主/从；
//   · 角色是**编译期**定死的（§5：`LINK_ROLE` 是唯一权威、运行期没有任何代码能改它）
//     ⇒ "这块板是谁"只存在于**刷进去的那份固件**里 ⇒ 那就让它自己说出来；
//   · 而**最该看到它的时刻**恰恰是刚烧完、还没接线、插上电那一秒 —— 也就是开机动画
//     那段时间（`BOOT_TOTAL_MS`，默认 ≈1.13 s）。
//
// ★★ 三条硬要求（都是车主点名的，别改）：
//   ① **只在开机动画期间显示**，动画一结束就消失 ⇒ 表盘保持干净。
//      实现上就是"可见性 = `g_boot.active(now)`"，改动只有**状态翻转的那一次**
//      （不每帧碰 LVGL：本驱动上多余的 invalidate 会把队列刷爆，灯条那一段踩过）。
//   ② **不许干扰既有的开机动画/表情逻辑**：本标签是一个**独立的顶层对象**，
//      不挂在 `face_bg`/`g_face_img` 上、不改它们的 opa、不参与扫表缓动。
//      ★ 特别是**不能**把 `face_apply()` 那条"表情显形"的路挡住 —— 2026-09-24
//      踩过"整个动画窗口一次都没轮到 boot_apply ⇒ 表情永久透明"那个坑
//      （见 `dash_ui_tick()` 里那段收尾补调的说明）；本标签**不碰**那条路径。
//   ③ **纯 ASCII**：本构建**只使能了 Montserrat 系列**（`include/lv_conf.h` 的
//      `LV_FONT_MONTSERRAT_*`）⇒ **没有 CJK 字形**。写成 "MASTER (RIGHT)" 是能画的，
//      写成"主板/右"就是**一片黑**（一个字形都画不出来）—— ★ 以后谁想把这两行
//      文案改成中文，先读这一条：那不是"看不清"，是**什么都没有**。
//      要中文就得先引 CJK 字体（`source_han_sans_sc_16_cjk`，占 Flash），那是独立一单。
//
// ★ 字号取 `READOUT_UNIT_FONT`（既有 24 号，与读数单位同一个）而不是更大的 48 号：
//   24 号在 480 档上已经足够醒目（一行 ≈ 200 px 宽），而 48 号会把这一行压到弧带上
//   （弧带在 181..205，本标签落在 y≈56..96 那条留白里，两个都不碰）。
static void build_role_label(lv_obj_t* parent, ScreenUi& ui) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  // ★ 文字**居中**且整行宽度铺满：字号/文案变了也不用重算位置。
  lv_obj_set_width(l, ts(480));
  lv_obj_set_pos(l, ts(0), ts(56));
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(l, READOUT_UNIT_FONT, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0xFFB020), 0);   // 琥珀（与诊断页描边同一色）
  // ★ 两行：第一行是**谁**，第二行是**它该装在哪一侧 + 跑哪份 env**。
  //   把 env 名写上去是有意的：烧错镜像时它是**唯一**能在屏上说清"你烧的是哪一份"的东西。
#if LINK_ROLE == 1
  lv_label_set_text(l, "MASTER (RIGHT)\nesp32s3-rgb-master");
#else
  lv_label_set_text(l, "SLAVE (LEFT)\nesp32s3-rgb-slave");
#endif
  ui.role_lbl = l;
  ui.role_visible = true;     // 建好就显示 —— 第一帧往往就落在开机动画里（见 role_apply）
}

// 把"这一拍该不该显示角色标签"落到像素上。**只在状态变了的时候碰 LVGL**（同 trust_apply）。
// ★ 判据就是**开机动画还在不在** —— 没有第二个条件、没有计时器、没有"显示 N 秒"。
static void role_apply(ScreenUi& ui, bool show) {
  if (!ui.role_lbl) return;
  if (show == ui.role_visible) return;
  ui.role_visible = show;
  if (show) lv_obj_remove_flag(ui.role_lbl, LV_OBJ_FLAG_HIDDEN);
  else      lv_obj_add_flag(ui.role_lbl, LV_OBJ_FLAG_HIDDEN);
}

// 把"这一拍该不该提示"落到像素上。**只在真的变了的时候碰 LVGL**：
//   角标是常态隐藏的，一旦显示就 5 Hz 在那儿 ⇒ 无脑每帧 set_text/set_style
//   会把 invalid 队列刷爆（灯条那一段的注释里踩过同一个坑）。
static void trust_apply(ScreenUi& ui, DataTrustReason r) {
  if (!ui.trust_badge) return;
  const uint8_t id = (uint8_t)r;
  if (id == ui.trust_last) return;
  const bool was_hidden = (ui.trust_last == 0xFF) || (ui.trust_last == 0u);
  ui.trust_last = id;
  if (r == DataTrustReason::kNone) {
    if (!was_hidden) lv_obj_add_flag(ui.trust_badge, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(ui.trust_text, trustBadgeText(r));
  lv_obj_set_style_border_color(ui.trust_badge, lv_color_hex(trustBadgeColor(r)), 0);
  lv_obj_center(ui.trust_text);
  lv_obj_remove_flag(ui.trust_badge, LV_OBJ_FLAG_HIDDEN);
}

// 建诊断页：**一块内缩的圆角面板**（不透明底 + 可见描边 + 标题 + 正文）。
// ★ 为什么是一整块而不是"在表盘上盖几个数字"：诊断页要能**明确地**盖住表盘
//   （否则"数字在动"与表盘上的弧混在一起，读数没法看），而且退出时必须是
//   "整块消失"——留半张脸在外面会让人以为界面坏了。
//   面板 opa 不透明（LV_OPA_COVER）：**不给它设半透明**（这个驱动上 opa<255
//   的整屏对象会开离屏层，层缓冲装不下整屏 ⇒ 下半屏回绕到顶部，踩过）。
//
// ★★ 2026-09-24 晚（车主原话："屏幕为什么黑了"）—— 这一版为什么换了底色：
//   上一版底是 **0x0A0A0A 铺满整屏**。哪怕字画对了，观感也就是"整屏黑了"
//   （实测那版页区域 96% 的采样点亮度 < 20）。而**底色深本身不是可读性**：
//   它必须在"这是一页界面"这件事上说话 ⇒ 现在做三件事：
//     ① **内缩**（不再铺满圆屏）⇒ 四周露出表盘底色，一眼看出"这是盖在表盘上的
//        一页"，而不是"屏坏了 / 板子死了"；
//     ② **可见描边**（琥珀 2px + 圆角）⇒ 哪怕一个字都没画出来，人也看得出
//        这里有"一页界面"，而不是一片黑。**这条是硬要求**（车主追加）；
//     ③ 底色抬到 0x16212E（亮度 ≈ 32，明显不是近黑）且与表盘底色 0x141414
//        区分得开 ⇒ 页区域平均亮度实测 ≈ 32（上一版 ≈ 10）。
//   ★ 为什么不再铺满：铺满的整屏不透明对象一开就是"整屏换了个颜色"，
//     圆屏上没有任何参照物 ⇒ 只能读成"黑了"。
//
// ★★ 字体纪律（这一页踩过两次，写在这里免得第三次）：
//   · 本构建**只使能了 Montserrat 系列**（include/lv_conf.h 的 LV_FONT_MONTSERRAT_*）
//     ⇒ **没有 CJK 字形**。诊断页的文本（含 `dataTrustReasonText()` 那条中文
//     短文）**必须一律 ASCII** —— 中文在这里不是"看不清"，是**一个字形都画不出来**，
//     结果是"一片黑、连一个字都没有"。
//   · 所以标题/页码/字段名全用 ASCII（DIAG 1/2、heap/psram/van/link…）。
//     真要用中文，先引入 CJK 字体（lvgl 自带 source_han_sans_sc_16_cjk，但要
//     占 Flash）——那是独立一单，不在本次改动里。
static void build_diag(lv_obj_t* parent, ScreenUi& ui) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  // ★ 尺寸/位置**显式写死**（绝不让 LVGL 自算），见下面 body 那段血泪说明。
  //   32 px 内缩：480 档四周各留 32，240 档按 ts() 自动减半。
  //   ★ 尺寸按 **480 基准** 写（ts(416) ⇒ 240 档上 208），不读 LV_HOR_RES：
  //     这个项目的几何只有"480 基准 + ts() 缩放"一套口径，随屏读分辨率会让
  //     240 档的面板变成"半屏"而不是等比缩小的面板。
  const int32_t kInset = ts(32);
  lv_obj_set_size(box, ts(416), ts(416));
  lv_obj_set_pos(box, kInset, kInset);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x16212E), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, ts(24), 0);
  lv_obj_set_style_border_width(box, ts(2), 0);
  lv_obj_set_style_border_color(box, lv_color_hex(0xFFB020), 0);   // 琥珀（与灯条/角标同色系）
  lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
  lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);    // ★ 平时**不显示**（产品要求）

  // 标题 + 页码：用**大数字那一档**字体（车主追加："标题与页码尤其要显眼"）。
  //   这一档在 480 上是 48 号（240 上是 24 号），是这块屏上最大的字。
  lv_obj_t* title = lv_label_create(box);
  lv_obj_remove_style_all(title);
  lv_obj_set_style_text_font(title, READOUT_DIGIT_FONT, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFB020), 0);
  // ★ 显式给尺寸与位置（不让 LVGL 自算）：
  //   自算尺寸的那条路在"多行文本 + 这个驱动"上出过问题（见下面 body 的说明），
  //   而这里每个标签要多大是**已知**的（一屏固定行数）⇒ 写死最稳。
  lv_obj_set_size(title, ts(384), ts(56));
  lv_obj_set_pos(title, ts(24), ts(16));
  lv_label_set_text(title, "DIAG 1/2");

  // 正文：多行文本。
  // ★★ 这里踩过一个坑（2026-09-24，写下来免得下次再花半小时）：
  //   第一版**没给标签设尺寸**（只设了字体/颜色/行距/位置），落到帧上
  //   **整屏全黑** —— 连表盘都没有了（预览的落盘帧里 96% 的采样点亮度 < 20）。
  //   原因是多行文本的高度由字体度量算、而这个精简版驱动上那条路没验过：
  //   标签自算出的尺寸把**整屏容器**撑出了屏幕，而容器是 LV_OPA_COVER 的
  //   ⇒ 一次整屏重绘把它自己（黑）盖满了。诊断页的文本是**定行数**的，
  //   所以尺寸本来就该写死：14 行 × 22 px 的行高放得下 480 档最长的两页。
  lv_obj_t* body = lv_label_create(box);
  lv_obj_remove_style_all(body);
  lv_obj_set_style_text_font(body, READOUT_UNIT_FONT, 0);
  // 正文抬到 0xE8EDF2（亮度 ≈ 235）：上一版 0xE0E0E0 在**真屏**上偏灰
  //   （面板不是显示器，暗部对比会再掉一档）⇒ 直接给到接近纯白。
  lv_obj_set_style_text_color(body, lv_color_hex(0xE8EDF2), 0);
  lv_obj_set_style_text_line_space(body, ts(6), 0);
  lv_obj_set_size(body, ts(368), ts(320));
  lv_obj_set_pos(body, ts(24), ts(80));
  lv_label_set_text(body, "");

  ui.diag_box = box;
  ui.diag_title = title;
  ui.diag_body = body;
  ui.diag_built = true;
}

// 把一页诊断页落到两个标签上。**逐字节比对**：文本没变就一个字节都不碰
//   （诊断页开着的时候是 5 Hz 重画，无脑 set_text 会把 invalid 队列刷爆）。
static void diag_apply(ScreenUi& ui, const SysStatusInputs& in, bool open) {
  if (!ui.diag_built) return;
  if (!open) {
    if (!lv_obj_has_flag(ui.diag_box, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_add_flag(ui.diag_box, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }
  const DiagView v = diagBuild(g_diag_page, in);
  char text[2048];
  diagRenderText(v, text, sizeof(text));
  if (strcmp(text, g_diag_text) != 0) {
    strncpy(g_diag_text, text, sizeof(g_diag_text) - 1);
    g_diag_text[sizeof(g_diag_text) - 1] = '\0';
    lv_label_set_text(ui.diag_body, g_diag_text);
  }
  // 标题带页号（"DIAG 1/2"）—— 一眼知道还有没有下一页
  char title[24];
  snprintf(title, sizeof(title), "%s %u/%u", v.title,
           (unsigned)((uint8_t)g_diag_page + 1u), (unsigned)kDiagPageCount);
  if (strcmp(title, g_diag_title) != 0) {
    strncpy(g_diag_title, title, sizeof(g_diag_title) - 1);
    g_diag_title[sizeof(g_diag_title) - 1] = '\0';
    lv_label_set_text(ui.diag_title, g_diag_title);
  }
  if (lv_obj_has_flag(ui.diag_box, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_remove_flag(ui.diag_box, LV_OBJ_FLAG_HIDDEN);
  }
  // ★ 显式把它抬到最上层：诊断页要盖住表盘（对象顺序就是图层顺序，
  //   而它是在读数**之后**建的 ⇒ 本来就在最上面；这一句是"万一有人
  //   在它之后又建了东西"的保险，代价是一次指针比较）。
  lv_obj_move_foreground(ui.diag_box);
}

// 诊断页的三个入口（main 把按键翻译成它们，见 dash_ui.h 的说明）
//
// ★★ 2026-09-24 晚：**每次打开都从第 1 页开始**（车主原话："关闭后重开停在第 2 页
//   这个别扭一并改掉"）。上一版页号**不被关闭动作复位** ⇒ 关掉再打开还在第 2 页
//   （预览的 `K` 与真机的 `d` 都是这么走的，两处语义**一致**，所以一起改）。
//   ★ 复位写在**打开的那一次**（而不是写在关闭的那一次）：
//     这样"关 → 开"与"开机后第一次开"走的是同一条路径，只有一处判据。
void dash_ui_diag_toggle() {
  g_diag_open = !g_diag_open;
  if (!g_diag_open) return;            // 关闭：什么都不动（页号留着，下一次打开会复位）
  g_diag_page = DiagPage::Sys;         // ★ 打开 ⇒ 一律第 1 页

  // ---- 几何自证（2026-09-24 晚，车主追加的要求）----
  // "诊断页一片黑、连一个字都没有"这件事**必须能从串口一眼判出来**，不必靠人眼猜：
  //   尺寸退化成 0 / 字体拿到 null / 盒子没建出来 —— 这一行三个都覆盖。
  // ★ 只打一次（不是每 5 Hz 一拍一行）：它与"页面上显示什么"无关，
  //   只回答"这一页的对象到底有多大、字用的是哪个字体"。
  //
  // ★★ 必须在**本次打开之后**读、而且要先 `lv_obj_update_layout()` ——
  //   这是 2026-09-24 上板实测踩到的：第一版把这一行写在"读到第一帧之前"，
  //   打出来的是 **`box=0x0 at 0,0`**（对象明明建好了、字体也对）。
  //   原因：LVGL 的 `lv_obj_get_width()/lv_obj_get_coords()` 读的是
  //   **上一次布局的结果**（默认布局是 `LV_LAYOUT_NONE` ⇒ 子对象的位置要等
  //   父对象第一次被**绘制**时才由 `lv_obj_refr_pos()` 落到 `coords` 上）。
  //   而 `build_diag()` 建完就是 HIDDEN、**一帧都没画过** ⇒ `coords` 全是 0。
  //   ⇒ 先让容器可见（这一步本来就在 diag_apply 里发生），再手动跑一次布局。
  //   ★ 这一条正是"几何自证"要防的那类事：**它自己先踩了一次**，
  //     而它在串口上把真相说出来了 —— 所以这一行必须留着。
  if (!g_diag_geom_logged && g_ui[0].diag_built) {
    g_diag_geom_logged = true;
    lv_obj_remove_flag(g_ui[0].diag_box, LV_OBJ_FLAG_HIDDEN);   // 先可见
    lv_obj_update_layout(g_ui[0].diag_box);                     // 再跑一次布局
    lv_area_t bc, tc, pc;
    lv_obj_get_coords(g_ui[0].diag_box, &bc);
    lv_obj_get_coords(g_ui[0].diag_title, &tc);
    lv_obj_get_coords(g_ui[0].diag_body, &pc);
    const lv_font_t* tf = lv_obj_get_style_text_font(g_ui[0].diag_title, LV_PART_MAIN);
    const lv_font_t* bf = lv_obj_get_style_text_font(g_ui[0].diag_body, LV_PART_MAIN);
    dash_logf("diag: geom box=%ldx%ld at %ld,%ld title=%ldx%ld at %ld,%ld "
              "body=%ldx%ld at %ld,%ld font title=%s/lh%ld body=%s/lh%ld page=1/%u\n",
              (long)lv_obj_get_width(g_ui[0].diag_box),
              (long)lv_obj_get_height(g_ui[0].diag_box),
              (long)bc.x1, (long)bc.y1,
              (long)lv_obj_get_width(g_ui[0].diag_title),
              (long)lv_obj_get_height(g_ui[0].diag_title),
              (long)tc.x1, (long)tc.y1,
              (long)lv_obj_get_width(g_ui[0].diag_body),
              (long)lv_obj_get_height(g_ui[0].diag_body),
              (long)pc.x1, (long)pc.y1,
              diagFontTag(tf), (long)(tf ? lv_font_get_line_height(tf) : 0),
              diagFontTag(bf), (long)(bf ? lv_font_get_line_height(bf) : 0),
              (unsigned)kDiagPageCount);
  }
}
void dash_ui_diag_next() {
  g_diag_page = (DiagPage)(((uint8_t)g_diag_page + 1u) % kDiagPageCount);
}
bool dash_ui_diag_open() { return g_diag_open; }
uint8_t dash_ui_diag_page() { return (uint8_t)g_diag_page; }

// ============ 开机动画应用(20ms 档推进,见 dash_ui_tick 的节流) ============
static void boot_apply(uint32_t now) {
  for (uint8_t s = 0; s < 2; ++s) {
    // 注意:不给屏幕对象设 opa<255 的淡入 —— LVGL 会因此给整屏渲染开离屏层,
    // 而层缓冲从显示缓冲里切(ARGB8888),装不下整屏 → 下半屏内容回绕到顶部。
    // 想要淡入效果时用一个不透明黑底覆盖件反向淡出,别动屏幕本身的透明度。
    ScreenUi& ui = g_ui[s];
    // ★ 扫表用的那一条弧必须与建屏时**同一套**（本单 B）：这里也走角色映射，
    //   否则从板的扫表会拿速度弧的几何去扫转速表的弧（槽位对不上 = 无动作/画错）。
    const uint8_t ti = dashlayout::themeIndexForScreen(s);
    const float p = g_boot.arcProgress(now, s);
    for (uint8_t i = 0; i < ui.arc_count; ++i) {
      ui.arc_cur[i] = p;   // 扫表直接跟随,结束后的数据缓动从这里起步
      arc_set_progress(ui.arcs[i], kScreens[ti].arcs[i], p);
    }

    if (kScreens[ti].show_face && (ui.face_bg || g_face_img[s])) {
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

// ============================================================
// ★★★ 图层顺序（**由创建顺序决定**，改顺序前先读这一条）★★★
//
// LVGL 里"后建的画在上面"（子对象顺序就是绘制顺序），而这个函数的每一段
//   都在建一层 ⇒ **本文件里段的先后 = 屏上谁压谁**。约定的最终层序
//   （2026-09-24 车主定稿，从下到上）：
//
//     1. 背景图            （最底，`g_bg_img`）
//     2. 表情              （程序化 `build_face` + 表情图片 `g_face_img`）
//     3. 表盘弧            （`build_arcs`）
//     4. 六格指示灯        （`build_lamps`）
//     5. 读数              （大数字 / 单位 / 水温 / 进气 —— **最高层**）
//
//   · 表情在弧**之下**：车主要"表盘弧永远是最上层"（原话），
//     代价是表情图的边缘会被弧裁掉一块 —— **这是设计如此**，不是 bug
//     （见 tools/theme-editor/asset-spec.js 的同一条注明）。
//   · 弧在表情**之上**、但在灯与读数**之下**：读数绝不许被弧压住
//     （车主原话："读数应该是最高层的"）。★ 别顺手把 `build_arcs` 挪到最后 ✗。
//   · 诊断页（最末尾建）**不在**这张表里：它平时 HIDDEN，一显示就要盖住
//     上面**全部**五层（整块不透明面板），所以它刻意建在读数之后。
// ============================================================
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

  // 图片资源:先探测**每一组**有没有图(没刷图片时全部 false,走降级路径)。
  //
  // ★★ 第一维是**表情分组**（表号），不是屏号 —— 取值只此一个来源：
  //   `faceGroupFor(s)`。★ 为什么必须由它来，而不是"顺手用循环变量 s"：
  //   从板上末屏（看得见的那一屏）的表号与屏号**正好对调**，
  //   用 `s` 就是把"左/转速那 5 张图"填进右边那一组 —— 而下面的
  //   `g_face_ok[group][…]` / 渲染层的取值都按**表号**读，
  //   于是那一屏的脸会按**另一块表的数据**挑图（车主原话："表情变化
  //   怎么感觉是随机变动的"，2026-09-26 实测报的就是这个）。
  for (uint8_t s = 0; s < 2; ++s) {
    const uint8_t g = faceGroupFor(s);
    g_ui[s].screen = s;
    g_ui[s].face_group = g;
    // 背景：两屏共用 1 号角色，所以两张 dsc 是同一张图（各自持有，互不影响）。
    g_bg_ok[g] = image_dsc_for_role(kBackgroundRole, &g_bg_dsc[g]);
    for (uint8_t slot = 0; slot < kFaceSlotCount; ++slot) {
      // 这组用不到的状态(角色号 0)不要去查图:查也查不到,但会把
      // "0 号角色"当成一个真实编号传下去,将来加角色时容易踩到。
      g_face_ok[g][slot] = faceSlotExists(g, slot) &&
                           image_dsc_for_role(faceRole(g, slot), &g_face_dsc[g][slot]);
    }
  }

  for (uint8_t s = 0; s < 2; ++s) {
    // ★★ 分组（本单 B 的唯一出口）：表盘 / 表情槽位 / 表情图片三条路共用这一个数。
    //   `dash_role_layout.h` 的 `faceGaugeIndexForScreen()` 就是
    //   `themeIndexForScreen()`（同一个式子，`test_role_layout` 钉住恒等）。
    const uint8_t g = g_ui[s].face_group;
    if (g_bg_ok[g]) {
      g_bg_img[s] = lv_image_create(g_screens[s]);
      lv_image_set_src(g_bg_img[s], &g_bg_dsc[g]);
      lv_obj_center(g_bg_img[s]);
    }

    // ---- 第 2 层：表情（程序化 + 图片）----
    // ★★ 表情有**两个**对象，两个都必须在 `build_arcs()` **之前**建，
    //   否则弧会被它们压住（2026-09-24 实测踩到：只把 `build_face()` 挪到前面
    //   还不够 —— `g_face_img` 那一步原先是**后面另一个循环**里建的，
    //   于是"图片表情"仍然盖在弧上；落帧上表现为弧被切掉四个缺口，
    //   而那个现象在"改成从下面建"之后**一个像素都没变**，正是这条没做全）。
    // ★★ 这一行（`kScreens` = `g_theme.screens`）是**本单 B 的落点**：
    //   "第 s 屏用哪一套表盘"现在由 `dash_role_layout.h` 说了算 ——
    //   主板（`LINK_ROLE==1`）⇒ 末屏 = `screens[1]`（速度表 + 进气温度）；
    //   从板（`LINK_ROLE==0`）⇒ 末屏 = `screens[0]`（转速表 + 水温）。
    //   ★ 判据不是"哪块玻璃"（今天两块屏指向同一个 `lv_display_t`），而是
    //     "**哪一屏是这一角色该显示的那一屏**" —— 理由与边界全在那个头文件里。
    //   ★ `g` 就是 `themeIndexForScreen(s)`（同一个数），所以这里直接用 `g`：
    //     **表盘与表情图片不许各算一次下标**（那正是上一版断掉的地方）。
    if (kScreens[g].show_face) build_face(g_screens[s], g_ui[s]);

    // 用图片表情替换(或隐藏)程序化表情。
    // ★ 降级:没有表情图时**保留程序化形状表情** —— 这条路径是刻意留的,
    //   与"没有主题就用默认主题"是同一个原则:资源缺失不能让界面空掉。
    bool any_face_img = false;
    for (uint8_t i = 0; i < kFaceSlotCount; ++i) any_face_img = any_face_img || g_face_ok[g][i];
    if (any_face_img) {
      g_face_img[s] = lv_image_create(g_screens[s]);
      lv_obj_center(g_face_img[s]);
      // 有图就把程序化表情藏起来(不能删 —— face_apply 还会去访问那几个对象)
      if (g_ui[s].face_bg) lv_obj_add_flag(g_ui[s].face_bg, LV_OBJ_FLAG_HIDDEN);
      // 先摆"常态该用的那张"(可能降级到别的槽),后续 face_apply 按状态切换
      const int slot = faceResolve(g, Face::Idle);
      g_face_slot[s] = (int8_t)slot;
      lv_image_set_src(g_face_img[s], &g_face_dsc[g][slot]);
    }

    // ---- 第 3 层：表盘弧（**永远在表情之上**，车主定稿）----
    build_arcs(g_screens[s], kScreens[g], g_ui[s]);
  }

  // 指示灯槽位(占位图形):建在**读数之前** —— 于是副表数字(水温/进气)
  // 压在灯条上面；两者在几何上不重叠(灯条 y=395..435、副表墨迹到 y≈396),
  // 所以图层顺序在这里只是"万一"的保险(见 ui_model.h 的 kLamp* 说明)。
  for (uint8_t s = 0; s < 2; ++s) {
    build_lamps(g_screens[s], g_ui[s]);
  }

  // 数字读数最后建:创建顺序就是图层顺序,读数要压在**弧、表情、灯**之上
  //   （车主 2026-09-24 定稿："读数应该是最高层的" ⇒ 弧被挪到表情之前之后，
  //     读数仍然是最后一个建的 —— 这一段的**位置**是层序约定的一部分，别动）。
  for (uint8_t s = 0; s < 2; ++s) {
    build_readout(g_screens[s], kScreens[dashlayout::themeIndexForScreen(s)], g_ui[s]);
  }

  // ★ 2026-09-24 新增的两个整屏/压边元素，**建在读数之后**（= 图层最上）：
  //   ① 数据不可信角标（右缘小图标，默认隐藏）
  //   ② 诊断页（整屏不透明容器，默认隐藏）
  //   顺序的理由：诊断页要能盖住表盘上的一切（包括读数），而角标要压在
  //   背景图与灯条之上。两条都靠"后建的在上面"这条 LVGL 规则，
  //   不额外调 move_foreground（那一句只在渲染时作保险，见 dash_ui_render）。
  for (uint8_t s = 0; s < 2; ++s) {
    build_trust_badge(g_screens[s], g_ui[s]);
    build_diag(g_screens[s], g_ui[s]);
    // ★★ 角色标签**最后建**（= 压在上面）：它只在开机动画那一秒多里出现，
    //   而那一秒里它是"这块板是谁"的唯一信息 ⇒ 不许被角标/诊断页的容器盖住。
    //   （诊断页建完是 HIDDEN，正常开机时它与本标签不共存。）
    build_role_label(g_screens[s], g_ui[s]);
  }

  // ★★ 2026-09-25（本单 B）：把"**这块板显示哪一屏**"打出来 —— 这是"角色 ↔ 屏幕"
  //   这条映射在串口上唯一能自证的一行（屏上什么样只能人眼看，而这一行说的是
  //   "我们**要求**它显示哪一套"）。★ 纯 ASCII（本构建只使能 Montserrat）。
  //   ★ `last` = 今天**唯一可见**的那一屏（RGB 单屏版本：`g_left`/`g_right` 指向同一个
  //     `lv_display_t`，后建的压在前面 ⇒ 末屏就是车主看到的那一屏）。第二块屏到货后
  //     这一行照旧成立（两屏各自的名字都在 `slots=` 里）。
  dash_logf("layout: role=%s last=%s slots=[%s | %s]\n",
            dashlayout::roleName(),
            dashlayout::panelNameForScreen(dashlayout::kScreenTop),
            dashlayout::panelNameForScreen(0u),
            dashlayout::panelNameForScreen(1u));

  // ★★ 2026-09-26（本单 B 的第二次修正）：把"**表情图片按哪一组取**"打出来。
  //   为什么非有这一行不可：车主第二次报的现象（"表情跟转速失去关联、像随机变"）
  //   在屏上**看不出是哪一环**（表盘是对的、读数是对的、脸也在变），
  //   而根因恰恰是"图片分组"这一环拿了**另一块表**的数据 ——
  //   它在串口上唯一能被看见的形态就是这两个数：**这一屏的表号 + 它的取图组号**。
  //   ★ 判据：`group` 必须等于同一行的 `panel=`（表号）——`test_role_layout`
  //     那组用例钉住的那条恒等，在这里以"运行期事实"的形式再出现一次。
  //   ★ 纯 ASCII；`idle_role=` 是**实际拿去 image_dsc_for_role() 的那个角色号**
  //     （不是表里的理论值）：有图就是它，没图就是 0 —— 一眼能看出"这一屏取的是
  //     左那 5 张还是右那 5 张"（左=3/12/13/21/4、右=6/22/17/18/8，见 image_blob.h）。
  for (uint8_t s = 0; s < 2; ++s) {
    const uint8_t g = g_ui[s].face_group;
    dash_logf("faceimg: screen=%u group=%u panel=%s gauge=%s idle_role=%u\n",
              (unsigned)s, (unsigned)g,
              dashlayout::panelNameForScreen(s),
              dashlayout::gaugeNameForScreen(s),
              (unsigned)kFaceRoleId[g][(uint8_t)Face::Idle]);
  }

  g_boot.start(millis());
  dash_logf("206 dash boot\n");
}

void dash_ui_tick(uint32_t now_ms) {
  if (last_tick_ms != 0) lv_tick_inc(now_ms - last_tick_ms);
  last_tick_ms = now_ms;

  // ★★ 角色标签的可见性 = **开机动画还在不在**（2026-09-25）。
  //   ★ 为什么放在最前面、而且**每拍都调**：它是"状态翻转的那一次才碰 LVGL"的
  //     （见 `role_apply`），所以每拍调用零代价；而放在前面能保证"动画结束的那一拍"
  //     就把它收掉，不必等渲染。
  //   ★ 预览专用的"把开机窗口钉住"那条钩子只有 pcpreview 有定义 ⇒ 这里必须仍是
  //     编译期的分叉：设备端那一行**一个符号都不引用**（与 `dash_display_preview_frames`
  //     等既有预览接口同一条纪律）。判据的主体（`g_boot.active`）两边是同一个表达式。
  bool boot_window = g_boot.active(now_ms);
#if defined(DASH_DISPLAY_PREVIEW)
  boot_window = boot_window || dash_display_preview_boot_hold();
#endif
  for (uint8_t s = 0; s < 2; ++s) role_apply(g_ui[s], boot_window);

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
    // ★★ 收尾必须再补一次 boot_apply（2026-09-24 实机踩到）：
    //   表情的"显形"不是靠 HIDDEN 标志，而是 boot_apply 里那一次
    //   opa TRANSP → COVER 的转移（`g_boot.faceStage()` 在 t≥FACE_START 才给 1）。
    //   而 boot_apply 只在 `g_boot.active(now)` 期间被调用 —— 于是只要**整个动画窗口
    //   里一次都没轮到**（主循环被"长 flush"挡住就会这样：RGB 那条路上，开机第一次
    //   整屏刷新要按块写、每块等一个消隐期，实测 ≈1 秒），那次转移就永远不会发生，
    //   表情**永久停在透明**上：弧、数字、灯都在，只有脸不见了 ✗
    //   （2026-09-24 就是这么丢了表情；预览那边 flush 是即时的，所以一直看不出来。）
    //   ⇒ 窗口结束后补调一次：此时 faceStage(t≥end) 已经是 1，状态被落到最终值。
    //     这条**与驱动无关**，是"动画状态机不能被主循环的卡顿跳过"该有的兜底。
    boot_apply(now_ms);
    // 把这次的档位一起打出来：faceStage=1 就是"这一次真的把表情的 opa 推到了 COVER"
    // ——它是"表情回来了"在串口上唯一能自证的证据（屏上什么样只有人眼能判）。
    dash_logf("boot anim done (收尾补一次 boot_apply: faceStage=%u → 表情 opa=COVER)\n",
              (unsigned)g_boot.faceStage(now_ms));
  }

  lv_timer_handler();
}

void dash_ui_render(const ArcDashView& v, const LampView& lamps, SystemStatus& sys,
                    const SysStatusInputs& diag, uint32_t now) {
  if (now - last_ok_ms >= 1000) {
    last_ok_ms = now;
    // face= 打的是**左/右两个**:两屏表情各看各的表,只打一个就分不清
    // 是"转速档没生效"还是"车速档没生效"。
    // ★ 2026-09-25（本单 B）：后面再挂一格 `role=` + `last=` —— 屏上跑的是哪一套表盘，
    //   这一行是**唯一**能在串口上看到它的地方（车主报的就是"从板刷成了速度表"，
    //   而那件事在这一行加这两格之前**一个字都看不出来**）。
    // ★★ 2026-09-26：**这一行降频到每 2 秒一条**（闸门见下）。
    //   它是本构建里"每秒一行"的**主要来源之一**（~90 B/行），内容却是缓变量
    //   （三个百分比 + 两个表情名）—— 1 Hz 是习惯，不是需求。
    //   ★ 它要回答的那两件事（`role=` / `last=`）一个字都没少，只是慢了一倍。
    if (g_ok_gate.take(now)) {
      dash_logf("206 dash ok  spd=%3.0f%% rpm=%3.0f%% coolant=%.0fC face=%s/%s | role=%s last=%s\n",
                    v.speed_t * 100.0f, v.rpm_t * 100.0f, v.coolant_c,
                    face_name(v.face_left), face_name(v.face_right),
                    dashlayout::roleName(),
                    dashlayout::panelNameForScreen(dashlayout::kScreenTop));
    }
  }

  // ---- ① 数据不可信状态机：**每拍都推进**（即使屏上什么都不显示）----
  // ★ 顺序是硬的：必须在画之前推进，否则这一拍显示的是上一拍的结论。
  // ★ 为什么在 dash_ui 里而不是 main 里：判据住在 lib/dashcore/（native 测掉），
  //   而"每 200 ms 推一次"这个节奏只有渲染这一层知道 ⇒ 放这里最不容易漏。
  //   主循环拿 `sys.beepDue()` 决定要不要响那一声（见 main.cpp）。
  const DataTrustReason trust = sys.update(diag, now);

  // ★ 灯条**在开机动画之前**应用:开机扫表期间也要能看见灯
  //   (打灯/开门是随时发生的,而开机动画只在前 1.1 秒)。它不参与弧的缓动。
  for (uint8_t s = 0; s < 2; ++s) lamp_apply(g_ui[s], lamps);

  // ---- ② 诊断页：开着的时候它**盖住一切**，所以先落它、再决定要不要画表盘 ----
  //   （诊断页的容器是不透明的整屏对象，落到最上层 ⇒ 表盘那些对象被它盖住，
  //     不必把表盘逐个隐藏 —— 那是"少改一处"的选择，也让退出是**整块消失**）
  for (uint8_t s = 0; s < 2; ++s) diag_apply(g_ui[s], diag, g_diag_open);

  if (g_boot.active(now)) return;   // 开机期间由 boot_apply 接管

  // ---- ③ 数据不可信角标（**只在不可信期间出现**，数据恢复自动消失）----
  // ★ 放在开机动画**之后**：开机那 1.1 秒里没有"数据"可言，挂个角标像故障。
  // ★ 诊断页开着时不画角标（诊断页第 1 页自己就有"数据可不可信"那一行）。
  for (uint8_t s = 0; s < 2; ++s) {
    trust_apply(g_ui[s], g_diag_open ? DataTrustReason::kNone : trust);
  }

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
    // ★★ 本单 B 的第二处：这一屏的弧几何、表情来源都按**角色映射**取。
    //   `ti` = 这一屏该用的主题下标（从板末屏 = 0 = 转速表 + 水温）。
    const uint8_t ti = dashlayout::themeIndexForScreen(s);
    for (uint8_t i = 0; i < ui.arc_count; ++i) {
      const ArcStyle& a = kScreens[ti].arcs[i];
      const float target = arc_progress(a, v);
      ui.arc_cur[i] += (target - ui.arc_cur[i]) * k;
      arc_set_progress(ui.arcs[i], a, ui.arc_cur[i]);
    }
    if (kScreens[ti].show_face && ui.face_bg) {
      // 表情的显隐/形变只在 face_apply 里按状态变化时改一次,这里不重复 set。
      // ★ 按屏取:**左屏用转速表的表情,右屏用速度表的表情**。
      //   ★ 2026-09-25：这里的 `s` 换成了 `ti` —— 与建屏时那一套表盘**同一个下标**
      //     （原来写死 `s == 0 ? 左 : 右`，那是"屏号 == 表盘号"的隐含假设，
      //      本单把角色映射引进来之后那个假设不再成立）。
      const Face f = (ti == 0u) ? v.face_left : v.face_right;
      face_apply(ui, f);
    }
    readout_apply(ui, v);
  }

  // ---- ④ 诊断页开着时，把它重新抬到最上层 ----
  // ★ 表盘那些 apply（弧/表情/读数）会碰对象，但**不会**改变创建顺序 ⇒ 严格说
  //   这一句是多余的；留着的理由是它把"诊断页必须盖住一切"这条不变式写在
  //   需要它的地方（将来谁在读数之后再建对象，这里就兜住了）。
  //   `lv_obj_move_foreground` 是纯指针操作（不触发重绘），代价可以忽略。
  if (g_diag_open) {
    for (uint8_t s = 0; s < 2; ++s) {
      if (g_ui[s].diag_box) lv_obj_move_foreground(g_ui[s].diag_box);
    }
  }
}

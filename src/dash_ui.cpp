#include "dash_ui.h"
#include "dash_display.h"
#include "ui_theme.h"
#include "boot_anim.h"
#include "image_load.h"     // 图片资源(背景图 / 表情图)
#include <lvgl.h>
#include <Arduino.h>
#include <math.h>

// ============ 运行时对象 ============
struct ScreenUi {
  uint8_t idx = 0;                  // 这是第几屏(0=左/车速,1=右/转速);图片按屏取
  lv_obj_t* arcs[kMaxArcs];
  uint8_t arc_count = 0;
  float arc_cur[kMaxArcs];          // 弧当前值(缓动用),开机扫表后从这里平滑过渡
  lv_obj_t* face_bg = nullptr;
  lv_obj_t* eye_l = nullptr;
  lv_obj_t* eye_r = nullptr;
  lv_obj_t* mouth = nullptr;
  Face last_face = Face::Count;     // 首帧强制全量应用
};

static ScreenUi g_ui[2];
static lv_obj_t* g_screens[2];
static BootAnim g_boot;
static bool g_boot_done_printed = false;
static uint32_t last_ok_ms = 0;
static uint32_t last_tick_ms = 0;

// ============ 图片资源 ============
// lv_image_dsc_t 必须由我们持有 —— LVGL 会一直引用它(set_src 不复制)。
// 每屏一张背景 + 三种表情状态,所以是 2 组。
static lv_image_dsc_t g_bg_dsc[2];
static lv_image_dsc_t g_face_dsc[2][3];      // [屏][0=常态 1=红区 2=惊喜]
static bool g_bg_ok[2] = {false, false};
static bool g_face_ok[2][3] = {{false, false, false}, {false, false, false}};
static lv_obj_t* g_bg_img[2] = {nullptr, nullptr};
static lv_obj_t* g_face_img[2] = {nullptr, nullptr};

// 表情状态 → 角色。
// ★ 按**法系车**布局:左屏(0) = 转速表,右屏(1) = 速度表。
//   所以"不带 R 后缀"的那组角色给左屏,"带 R"的给右屏 ——
//   这里的映射决定了刷进去的表情图会不会左右颠倒。
static ImageRole faceRole(uint8_t screen, int state) {
  if (screen == 0) {                 // 左屏 = 转速表
    return (state == 0) ? ImageRole::FaceIdle
         : (state == 1) ? ImageRole::FaceRedline
                        : ImageRole::FaceSurprise;
  }
  return (state == 0) ? ImageRole::FaceIdleR      // 右屏 = 速度表
       : (state == 1) ? ImageRole::FaceRedlineR
                      : ImageRole::FaceSurpriseR;
}

// 当前该显示哪一种表情。ExpressionState 与 Face 的映射沿用现有逻辑,
// 这里只做"状态 → 数组下标"的换算。
static int faceStateIndex(Face f) {
  switch (f) {
    case Face::Redline:  return 1;
    case Face::Surprise: return 2;
    default:             return 0;
  }
}

// 主题尺寸换算:480 基准 → 实际分辨率(四舍五入,见 ui_theme.h 分辨率适配)
static int32_t ts(float v480) {
  return (int32_t)lroundf(v480 * theme_scale());
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
    lv_arc_set_start_angle(arc, a.start_deg);
    lv_arc_set_end_angle(arc, a.start_deg);   // 初始 0 进度

    lv_obj_set_style_arc_width(arc, ts(a.width), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ts(a.width), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, a.track_color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, a.track_opa, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, a.value_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
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

// 表情状态 → 图片槽位下标。
// Blink / Cruise / Sport 都归到"常态"那张图 —— 逐帧眨眼动画不做(已确认),
// 所以这几种状态共用同一张脸,只有红区/惊喜才换图。
static int faceSlot(Face f) {
  switch (f) {
    case Face::Redline:  return 1;
    case Face::Surprise: return 2;
    default:             return 0;   // Idle / Blink / Cruise / Sport
  }
}

// 有图片表情时,只切图、不碰程序化形状。
// 返回 true 表示这次由图片接管了。
static bool face_apply_image(uint8_t screen, Face f) {
  if (g_face_img[screen] == nullptr) return false;
  const int slot = faceSlot(f);
  if (!g_face_ok[screen][slot]) return false;      // 这个状态没有图
  lv_image_set_src(g_face_img[screen], &g_face_dsc[screen][slot]);
  return true;
}

static void face_apply(ScreenUi& ui, Face f) {
  if (f == ui.last_face) return;
  ui.last_face = f;

  // ★ 有图片表情时由图片接管,程序化形状保持隐藏。
  //   注意 early return 必须在 last_face 更新之后 —— 否则每次都会重复判定。
  if (face_apply_image(ui.idx, f)) return;

  const bool blink = (f == Face::Blink);
  const bool surprise = (f == Face::Surprise);
  const bool narrow =
      (f == Face::Cruise || f == Face::Sport || f == Face::Redline);

  lv_obj_set_style_bg_color(ui.face_bg,
                            (f == Face::Redline) ? FACE_BG_REDLINE : FACE_BG_IDLE, 0);

  if (blink) {
    lv_obj_add_flag(ui.eye_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.eye_r, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(ui.eye_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui.eye_r, LV_OBJ_FLAG_HIDDEN);
    const uint8_t w = surprise ? EYE_SURPRISE : EYE_NORMAL_W;
    const uint8_t h = surprise ? EYE_SURPRISE : (narrow ? EYE_NARROW_H : EYE_NORMAL_H);
    lv_obj_set_size(ui.eye_l, ts(w), ts(h));
    lv_obj_set_size(ui.eye_r, ts(w), ts(h));
    lv_obj_set_style_radius(ui.eye_l, ts(h / 2), 0);
    lv_obj_set_style_radius(ui.eye_r, ts(h / 2), 0);
  }

  if (surprise) {
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
      const ArcStyle& a = kScreens[s].arcs[i];
      lv_arc_set_end_angle(ui.arcs[i],
                           a.start_deg + (int32_t)(p * (a.end_deg - a.start_deg)));
    }

    if (kScreens[s].show_face && (ui.face_bg || g_face_img[s])) {
      // 表情睁眼:阶段 0 透明、之后全显。只在阶段切换时 set 一次,
      // 避免每 tick 重复 set opa 触发无谓重绘(曾导致层合成异常)。
      const uint8_t st = g_boot.faceStage(now);
      static uint8_t last_st[2] = {0xFF, 0xFF};
      if (st != last_st[s]) {
        last_st[s] = st;
        const lv_opa_t opa = (st == 0) ? LV_OPA_TRANSP : LV_OPA_COVER;
        if (ui.face_bg) lv_obj_set_style_opa(ui.face_bg, opa, 0);
        // ★ 有图片表情时淡入要作用在图片上 —— 否则"睁眼"这个开机动作
        //   在有图的情况下会完全消失(程序化那层被藏起来了)。
        if (g_face_img[s]) lv_obj_set_style_opa(g_face_img[s], opa, 0);
      }
      if (st == 1) face_apply(ui, Face::Blink);
      else if (st == 2) face_apply(ui, Face::Idle);
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
    for (int st = 0; st < 3; ++st) {
      g_face_ok[s][st] = image_dsc_for_role(faceRole(s, st), &g_face_dsc[s][st]);
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
    for (int st = 0; st < 3; ++st) any = any || g_face_ok[s][st];
    if (!any) continue;

    if (g_face_img[s] == nullptr) {
      g_face_img[s] = lv_image_create(g_screens[s]);
      lv_obj_center(g_face_img[s]);
    }
    // 有图就把程序化表情藏起来(不能删 —— face_apply 还会去访问那几个对象)
    if (g_ui[s].face_bg) lv_obj_add_flag(g_ui[s].face_bg, LV_OBJ_FLAG_HIDDEN);
    // 先摆常态那张,后续 face_apply 按状态切换
    lv_image_set_src(g_face_img[s], &g_face_dsc[s][0]);
  }

  g_boot.start(millis());
  Serial.println("206 dash boot");
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
    Serial.println("boot anim done");
  }

  lv_timer_handler();
}

void dash_ui_render(const ArcDashView& v, uint32_t now) {
  if (now - last_ok_ms >= 1000) {
    last_ok_ms = now;
    Serial.printf("206 dash ok  spd=%3.0f%% rpm=%3.0f%% coolant=%.0fC face=%s\n",
                  v.speed_t * 100.0f, v.rpm_t * 100.0f,
                  v.coolant_c, face_name(v.face));
  }

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
      lv_arc_set_end_angle(ui.arcs[i],
                           a.start_deg +
                               (int32_t)(ui.arc_cur[i] * (a.end_deg - a.start_deg)));
    }
    if (kScreens[s].show_face && ui.face_bg) {
      // 表情的显隐/形变只在 face_apply 里按状态变化时改一次,这里不重复 set
      face_apply(ui, v.face);
    }
  }
}

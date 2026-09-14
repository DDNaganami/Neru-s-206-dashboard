#include "dash_ui.h"
#include "dash_display.h"
#include "ui_theme.h"
#include "boot_anim.h"
#include <lvgl.h>
#include <Arduino.h>

// ============ 运行时对象 ============
struct ScreenUi {
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

// ============ 屏幕与控件构建 ============
static lv_obj_t* make_screen(lv_display_t* disp) {
  lv_display_set_default(disp);
  lv_obj_t* scr = lv_obj_create(nullptr);
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
    lv_obj_set_size(arc, a.radius * 2, a.radius * 2);

    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_start_angle(arc, a.start_deg);
    lv_arc_set_bg_end_angle(arc, a.end_deg);
    lv_arc_set_start_angle(arc, a.start_deg);
    lv_arc_set_end_angle(arc, a.start_deg);   // 初始 0 进度

    lv_obj_set_style_arc_width(arc, a.width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, a.width, LV_PART_INDICATOR);
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
  lv_obj_center(bg);
  lv_obj_set_size(bg, THEME_FACE_SIZE, THEME_FACE_SIZE);
  lv_obj_set_style_bg_color(bg, FACE_BG_IDLE, 0);
  lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bg, THEME_FACE_SIZE / 2, 0);

  lv_obj_t* eye_l = lv_obj_create(bg);
  lv_obj_t* eye_r = lv_obj_create(bg);
  for (lv_obj_t* e : {eye_l, eye_r}) {
    lv_obj_remove_style_all(e);
    lv_obj_set_style_bg_color(e, FACE_INK, 0);
    lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
  }
  lv_obj_set_pos(eye_l, FACE_EYE_L_X, FACE_EYE_Y);
  lv_obj_set_pos(eye_r, FACE_EYE_R_X, FACE_EYE_Y);

  lv_obj_t* mouth = lv_obj_create(bg);
  lv_obj_remove_style_all(mouth);
  lv_obj_set_style_bg_color(mouth, FACE_INK, 0);

  ui.face_bg = bg;
  ui.eye_l = eye_l;
  ui.eye_r = eye_r;
  ui.mouth = mouth;
}

static void face_apply(ScreenUi& ui, Face f) {
  if (f == ui.last_face) return;
  ui.last_face = f;

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
    lv_obj_set_size(ui.eye_l, w, h);
    lv_obj_set_size(ui.eye_r, w, h);
    lv_obj_set_style_radius(ui.eye_l, h / 2, 0);
    lv_obj_set_style_radius(ui.eye_r, h / 2, 0);
  }

  if (surprise) {
    lv_obj_set_pos(ui.mouth, MOUTH_O_X, MOUTH_O_Y);
    lv_obj_set_size(ui.mouth, MOUTH_O_SIZE, MOUTH_O_SIZE);
    lv_obj_set_style_radius(ui.mouth, MOUTH_O_SIZE / 2, 0);
    lv_obj_set_style_bg_opa(ui.mouth, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.mouth, 4, 0);
    lv_obj_set_style_border_color(ui.mouth, FACE_INK, 0);
  } else {
    lv_obj_set_pos(ui.mouth, MOUTH_LINE_X, MOUTH_LINE_Y);
    lv_obj_set_size(ui.mouth, MOUTH_LINE_W, MOUTH_LINE_H);
    lv_obj_set_style_radius(ui.mouth, MOUTH_LINE_H / 2, 0);
    lv_obj_set_style_bg_opa(ui.mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.mouth, 0, 0);
  }
}

// ============ 开机动画应用(每个主循环都跑,保证扫表流畅) ============
static void boot_apply(uint32_t now) {
  for (uint8_t s = 0; s < 2; ++s) {
    lv_obj_set_style_opa(g_screens[s], g_boot.screenOpa(now), 0);

    ScreenUi& ui = g_ui[s];
    const float p = g_boot.arcProgress(now, s);
    for (uint8_t i = 0; i < ui.arc_count; ++i) {
      ui.arc_cur[i] = p;   // 扫表直接跟随,结束后的数据缓动从这里起步
      const ArcStyle& a = kScreens[s].arcs[i];
      lv_arc_set_end_angle(ui.arcs[i],
                           a.start_deg + (int32_t)(p * (a.end_deg - a.start_deg)));
    }

    if (kScreens[s].show_face) {
      const uint8_t st = g_boot.faceStage(now);
      lv_obj_set_style_opa(ui.face_bg,
                           (st == 0) ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
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
  lv_init();
  dash_display_init();

  lv_display_t* def = lv_display_get_default();
  g_screens[0] = make_screen(dash_display_left());
  g_screens[1] = make_screen(dash_display_right());
  lv_display_set_default(def);

  for (uint8_t s = 0; s < 2; ++s) {
    build_arcs(g_screens[s], kScreens[s], g_ui[s]);
    if (kScreens[s].show_face) build_face(g_screens[s], g_ui[s]);
  }

  g_boot.start(millis());
  Serial.println("206 dash boot");
}

void dash_ui_tick(uint32_t now_ms) {
  if (last_tick_ms != 0) lv_tick_inc(now_ms - last_tick_ms);
  last_tick_ms = now_ms;

  if (g_boot.active(now_ms)) {
    boot_apply(now_ms);   // 开机动画按主循环频率刷,不受 5Hz 渲染节流
  } else if (!g_boot_done_printed) {
    g_boot_done_printed = true;
    Serial.println("boot anim done");
  }

  lv_timer_handler();
}

void dash_ui_render(const ArcDashView& v) {
  const uint32_t now = millis();
  if (now - last_ok_ms >= 1000) {
    last_ok_ms = now;
    Serial.printf("206 dash ok  spd=%3.0f%% rpm=%3.0f%% coolant=%.0fC face=%s\n",
                  v.speed_t * 100.0f, v.rpm_t * 100.0f,
                  v.coolant_c, face_name(v.face));
  }

  if (g_boot.active(now)) return;   // 开机期间由 boot_apply 接管

  for (uint8_t s = 0; s < 2; ++s) {
    ScreenUi& ui = g_ui[s];
    for (uint8_t i = 0; i < ui.arc_count; ++i) {
      const ArcStyle& a = kScreens[s].arcs[i];
      const float target = arc_progress(a, v);
      ui.arc_cur[i] += (target - ui.arc_cur[i]) * kArcSmooth;   // 缓动
      lv_arc_set_end_angle(ui.arcs[i],
                           a.start_deg +
                               (int32_t)(ui.arc_cur[i] * (a.end_deg - a.start_deg)));
    }
    if (kScreens[s].show_face) {
      lv_obj_set_style_opa(ui.face_bg, LV_OPA_COVER, 0);
      face_apply(ui, v.face);
    }
  }
}

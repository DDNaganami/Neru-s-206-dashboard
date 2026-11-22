#include "nav_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dash_log.h"

// ============================================================
// 导航页（左屏）—— 阶段 A：几何占位 + 文案
//
// 只看这一个文件就知道"屏上有什么"：容器 / 转向三角 / 距离 / 车速 / 车标 / NO NAV。
// 数据判据全在 `nav_state.h`，这里**不判断**新鲜度（`s.valid` 是唯一入口）。
//
// ★ 颜色不走主题（阶段 A）：导航页是**新形态**，主题 R6 的 `layout=nav` 与
//   图片角色 24 是阶段 B 的事。现在固定一套高对比深色，等图片指针到位再搬进主题。
// ============================================================

namespace {

constexpr int kRes = 480;

// 配色（阶段 A 固定；夜间模式上来时整体再压亮度，见 panel guard 的背光）
constexpr uint32_t kBgDark   = 0x0E1116u;
constexpr uint32_t kInk       = 0xE8EDF2u;
constexpr uint32_t kDim       = 0x6B7785u;
constexpr uint32_t kAccent    = 0x35C6F4u;   // 转向箭头
constexpr uint32_t kWarn      = 0xF2545Fu;   // 掉头/到达
constexpr uint32_t kSpeedCol  = 0x62D995u;

struct Ui {
  lv_obj_t* root = nullptr;
  lv_obj_t* arrow = nullptr;
  lv_obj_t* dist = nullptr;
  lv_obj_t* unit = nullptr;
  lv_obj_t* speed = nullptr;
  lv_obj_t* speed_unit = nullptr;
  lv_obj_t* path = nullptr;   // 示意路径（不是真地图：高德广播不给坐标）
  lv_obj_t* car = nullptr;
  lv_obj_t* nonav = nullptr;
  NavRoute route;      // 阶段 B：路线几何（手机发来的局部坐标，单位米）

  // 缓存："值变了才重排"
  bool     have = false;
  bool     valid = false;
  uint8_t  turn = 0xFF;
  uint16_t dist_m = 0xFFFF;
  uint8_t  speed_kph = 0xFF;
  uint16_t heading = 0xFFFF;   // 量化到 2°
  int      angle = 999;        // 画出来的箭头角度（度）
};

Ui g;

// 绕 (cx,cy) 旋转 (x,y) 角度（度，顺时针为正；屏坐标 y 向下）
lv_point_precise_t rot(int cx, int cy, float x, float y, float deg) {
  const float r = deg * 3.14159265f / 180.0f;
  const float c = cosf(r), s = sinf(r);
  lv_point_precise_t p;
  p.x = (lv_value_precise_t)(cx + (int)(x * c - y * s));
  p.y = (lv_value_precise_t)(cy + (int)(x * s + y * c));
  return p;
}

// `NavTurn` → 箭头朝向（0 = 上，顺时针为正）
int turn_angle(uint8_t turn) {
  switch (static_cast<NavTurn>(turn)) {
    case NavTurn::Left:       return -90;
    case NavTurn::Right:      return 90;
    case NavTurn::LeftAhead:  return -45;
    case NavTurn::RightAhead: return 45;
    case NavTurn::UTurn:      return 180;
    case NavTurn::RampIn:     return -30;
    case NavTurn::RampOut:    return 30;
    case NavTurn::Arrive:     return 0;
    case NavTurn::Straight:
    case NavTurn::Waypoint:
    case NavTurn::Unknown:
    default:                  return 0;
  }
}

bool turn_is_warn(uint8_t turn) {
  return turn == static_cast<uint8_t>(NavTurn::UTurn) ||
         turn == static_cast<uint8_t>(NavTurn::Arrive);
}

void arrow_draw_cb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const int cx = (a.x1 + a.x2) / 2;
  const int cy = (a.y1 + a.y2) / 2;
  const float h = (float)((a.y2 - a.y1) / 2);
  const float ang = (float)(g.angle == 999 ? 0 : g.angle);
  const lv_color_t col = lv_color_hex(turn_is_warn(g.turn) ? kWarn : kAccent);

  lv_draw_triangle_dsc_t head;
  lv_draw_triangle_dsc_init(&head);
  head.p[0] = rot(cx, cy, 0.0f, -h, ang);
  head.p[1] = rot(cx, cy, h * 0.62f, h * 0.10f, ang);
  head.p[2] = rot(cx, cy, -h * 0.62f, h * 0.10f, ang);
  head.color = col;
  head.opa = LV_OPA_COVER;
  lv_draw_triangle(layer, &head);

}

// 示意路径：底部竖线 + 到路口按 turn 拐弯（左/右/掉头/直行）。
// ★ 为什么是"示意"：高德广播只给转向图标与距离，**不给路线坐标**（见
//   docs/BRANCH-NAV-FEASIBILITY.md §11）。真路径要等阶段 B（自研 App + 高德 SDK）。
void path_draw_cb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const int cx = (a.x1 + a.x2) / 2;
  const int yb = a.y2;               // 底部（靠车图标）
  const int yt = a.y1;               // 顶部（前方）
  const int bend = (yt + yb) / 2;    // 拐弯高度

  lv_draw_line_dsc_t ld;
  lv_draw_line_dsc_init(&ld);
  ld.color = lv_color_hex(kAccent);
  ld.width = 10;
  ld.opa = LV_OPA_80;

  // 阶段 B：有真实路线点就画它（heading-up：车头朝上）
  if (g.route.valid && g.route.count >= 2u) {
    const float hd = (float)(g.heading / 100.0f) * 3.14159265f / 180.0f;
    const float ch = cosf(hd), sh = sinf(hd);
    lv_draw_line_dsc_t rl;
    lv_draw_line_dsc_init(&rl);
    rl.color = lv_color_hex(kAccent);
    rl.width = 12;
    rl.opa = LV_OPA_90;
    const int scale = 2;                 // 1 米 -> 2 px（近处这段路）
    int px = 0, py = 0;
    for (uint8_t i = 0; i < g.route.count; ++i) {
      // 旋转：把"右/前"的局部坐标转到 heading-up 屏幕坐标
      const float mx = (float)g.route.pts[i].dx * (float)scale;
      const float my = (float)g.route.pts[i].dy * (float)scale;
      const int sx2 = cx + (int)(mx * ch - my * sh);
      const int sy2 = (yb - 20) + (int)(mx * sh + my * ch);
      if (i > 0) {
        rl.p1.x = px; rl.p1.y = py;
        rl.p2.x = sx2; rl.p2.y = sy2;
        lv_draw_line(layer, &rl);
      }
      px = sx2; py = sy2;
    }
    return;
  }

  const int dir = turn_angle(g.turn);   // 0=直行 -90=左 90=右 180=掉头
  if (dir == 0) {
    ld.p1.x = cx; ld.p1.y = yb;
    ld.p2.x = cx; ld.p2.y = yt;
    lv_draw_line(layer, &ld);
  } else if (dir == 180) {
    ld.p1.x = cx; ld.p1.y = yb; ld.p2.x = cx; ld.p2.y = bend;
    lv_draw_line(layer, &ld);
    ld.p1.x = cx; ld.p1.y = bend; ld.p2.x = cx + 70; ld.p2.y = bend;
    lv_draw_line(layer, &ld);
  } else {
    const int sx = (dir > 0) ? 70 : -70;   // 右转往右拐
    ld.p1.x = cx; ld.p1.y = yb; ld.p2.x = cx; ld.p2.y = bend;
    lv_draw_line(layer, &ld);
    ld.p1.x = cx; ld.p1.y = bend; ld.p2.x = cx + sx; ld.p2.y = bend;
    lv_draw_line(layer, &ld);
    ld.p1.x = cx + sx; ld.p1.y = bend; ld.p2.x = cx + sx; ld.p2.y = yt;
    lv_draw_line(layer, &ld);
  }
}

void car_draw_cb(lv_event_t* e) {
  lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
  lv_layer_t* layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const int cx = (a.x1 + a.x2) / 2;
  const int cy = (a.y1 + a.y2) / 2;
  const float R = (float)((a.x2 - a.x1) / 2);
  const float ang = (float)(g.heading / 100.0f);   // 0.01deg -> deg

  lv_draw_rect_dsc_t body;
  lv_draw_rect_dsc_init(&body);
  body.bg_color = lv_color_hex(kAccent);
  body.bg_opa = LV_OPA_60;
  body.radius = LV_RADIUS_CIRCLE;
  lv_area_t ring;
  ring.x1 = cx - (int)(R * 0.56f);
  ring.y1 = cy - (int)(R * 0.56f);
  ring.x2 = cx + (int)(R * 0.56f);
  ring.y2 = cy + (int)(R * 0.56f);
  lv_draw_rect(layer, &body, &ring);
  // 车标（阶段 A 几何占位）：车头三角 + 半透明圆环，整体随 heading 旋转。
  lv_draw_triangle_dsc_t nose;
  lv_draw_triangle_dsc_init(&nose);
  nose.p[0] = rot(cx, cy, 0.0f, -R * 0.92f, ang);
  nose.p[1] = rot(cx, cy, R * 0.46f, -R * 0.10f, ang);
  nose.p[2] = rot(cx, cy, -R * 0.46f, -R * 0.10f, ang);
  nose.color = lv_color_hex(kInk);
  nose.opa = LV_OPA_COVER;
  lv_draw_triangle(layer, &nose);

}

void make_container(lv_obj_t* parent) {
  g.root = lv_obj_create(parent);
  lv_obj_remove_style_all(g.root);
  lv_obj_set_size(g.root, kRes, kRes);
  lv_obj_set_pos(g.root, 0, 0);
  lv_obj_clear_flag(g.root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(g.root, lv_color_hex(kBgDark), 0);
  lv_obj_set_style_bg_opa(g.root, LV_OPA_COVER, 0);
}

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int x, int y) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_pos(l, x, y);
  return l;
}

}  // namespace

void nav_ui_attach(lv_display_t* disp) {
  if (g.root) return;   // 只建一次
  lv_obj_t* scr = disp ? lv_display_get_screen_active(disp) : lv_screen_active();
  if (!scr) return;
  make_container(scr);

  // 转向箭头（上方）
  g.arrow = lv_obj_create(g.root);
  lv_obj_remove_style_all(g.arrow);
  lv_obj_set_size(g.arrow, 200, 200);
  lv_obj_set_pos(g.arrow, (kRes - 200) / 2, 26);
  lv_obj_add_event_cb(g.arrow, arrow_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

  // 示意路径（车图标上方那一段）
  g.path = lv_obj_create(g.root);
  lv_obj_remove_style_all(g.path);
  lv_obj_set_size(g.path, 260, 74);
  lv_obj_set_pos(g.path, (kRes - 260) / 2, 250);
  lv_obj_add_event_cb(g.path, path_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

  // 距离（大号数字 + 单位）
  g.dist = make_label(g.root, &lv_font_montserrat_48, kInk, 0, 168);
  lv_obj_set_width(g.dist, kRes);
  lv_obj_set_style_text_align(g.dist, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g.dist, "---");
  g.unit = make_label(g.root, &lv_font_montserrat_24, kDim, 0, 220);
  lv_obj_set_width(g.unit, kRes);
  lv_obj_set_style_text_align(g.unit, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g.unit, "");

  // 车标（随 heading 旋转）
  g.car = lv_obj_create(g.root);
  lv_obj_remove_style_all(g.car);
  lv_obj_set_size(g.car, 120, 120);
  lv_obj_set_pos(g.car, (kRes - 120) / 2, 316);
  lv_obj_add_event_cb(g.car, car_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

  // 车速（右下）
  g.speed = make_label(g.root, &lv_font_montserrat_48, kSpeedCol, 300, 356);
  lv_label_set_text(g.speed, "--");
  g.speed_unit = make_label(g.root, &lv_font_montserrat_14, kDim, 300, 408);
  lv_label_set_text(g.speed_unit, "km/h");

  // "没有导航数据"
  g.nonav = make_label(g.root, &lv_font_montserrat_24, kWarn, 0, 210);
  lv_obj_set_width(g.nonav, kRes);
  lv_obj_set_style_text_align(g.nonav, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(g.nonav, "NO NAV");
  lv_obj_add_flag(g.nonav, LV_OBJ_FLAG_HIDDEN);
}

void nav_ui_update(const NavState& s) {
  if (!g.root) return;

  const bool valid = s.valid;
  if (g.have && g.valid == valid && g.turn == s.turn && g.dist_m == s.dist_m &&
      g.speed_kph == s.speed_kph && g.heading == s.heading_cdeg) {
    return;   // 一个字节都没变 ⇒ 连重绘都不发
  }
  g.have = true;
  g.valid = valid;
  g.route = s.route;

  // 没有数据：整页只留 "NO NAV"（不许显示过期数据）
  if (!valid) {
    lv_obj_add_flag(g.arrow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.path, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.dist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.unit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.car, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.speed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.speed_unit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g.nonav, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_clear_flag(g.arrow, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g.path, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g.dist, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g.unit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g.car, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g.speed, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g.speed_unit, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g.nonav, LV_OBJ_FLAG_HIDDEN);

  // 箭头角度
  const int ang = turn_angle(s.turn);
  if (ang != g.angle) {
    g.angle = ang;
    lv_obj_invalidate(g.arrow);
    lv_obj_invalidate(g.path);   // 路径的拐弯方向跟着 turn 变
  }

  // 距离文案："300" + "m" / "1.2" + "km"
  if (s.dist_m != g.dist_m) {
    g.dist_m = s.dist_m;
    bool km = false;
    const uint32_t v = nav_dist_value(s.dist_m, &km);
    char buf[16];
    if (km) snprintf(buf, sizeof(buf), "%u.%u", (unsigned)(v / 10u), (unsigned)(v % 10u));
    else    snprintf(buf, sizeof(buf), "%u", (unsigned)v);
    lv_label_set_text(g.dist, buf);
    lv_label_set_text(g.unit, km ? "km" : "m");
  }

  // 车速
  if (s.speed_kph != g.speed_kph) {
    g.speed_kph = s.speed_kph;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)s.speed_kph);
    lv_label_set_text(g.speed, buf);
  }

  // 车标旋转（量化到 1°：0.01° 的抖动没必要上屏）
  const uint16_t hq = (uint16_t)((s.heading_cdeg / 100u) * 100u);
  if (hq != g.heading) {
    g.heading = hq;
    lv_obj_invalidate(g.car);
  }
}

#if defined(DASH_DISPLAY_PREVIEW)
void nav_ui_preview_tick(uint32_t now_ms) {
  if (!g.root) return;
  // 合成一串"像在导航"的数据：车头慢慢转、距离循环、转向每 4 s 换一种。
  NavState s;
  s.valid       = true;
  s.heading_cdeg = (uint16_t)((now_ms / 40u) % 36000u);          // 10 s 转一圈
  s.dist_m      = (uint16_t)(1000u - ((now_ms / 100u) % 1000u)); // 1000→1 m 循环
  s.speed_kph   = (uint8_t)(40u + (now_ms / 500u) % 30u);
  static const uint8_t kTurns[] = {(uint8_t)NavTurn::Straight, (uint8_t)NavTurn::Left,
                                   (uint8_t)NavTurn::Right, (uint8_t)NavTurn::UTurn,
                                   (uint8_t)NavTurn::RampIn, (uint8_t)NavTurn::Arrive};
  s.turn = kTurns[(now_ms / 4000u) % (sizeof(kTurns) / sizeof(kTurns[0]))];
  // 阶段 B：合成一条"S 形"路线（局部坐标，米），验证真路线渲染
  s.route.valid = true;
  s.route.count = 10;
  for (uint8_t i = 0; i < s.route.count; ++i) {
    s.route.pts[i].dx = (int16_t)((int)i * 6 - 30);
    s.route.pts[i].dy = (int16_t)(-(int)i * 18);
  }
  nav_ui_update(s);
}
#endif

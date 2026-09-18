#include "ui_theme.h"
#include <lvgl.h>

// 数字读数用哪号字体。
// 用编号而不是指针:指针没法存进 JSON,而主题要从 flash 的 JSON 加载。
// 打开的字号见 include/lv_conf.h(48 / 18 / 14)。
const lv_font_t* readout_font(uint8_t which) {
  switch (which) {
    case 0: return &lv_font_montserrat_48;
    case 1: return &lv_font_montserrat_18;
    default: return &lv_font_montserrat_18;
  }
}

// 默认主题:const 放 flash(.rodata),不占 DRAM。
// 函数内 static 保证只初始化一份,且首次使用时才初始化。
const Theme& theme_defaults() {
  static const Theme d = [] {
    Theme t;
    theme_set_defaults(t);
    theme_clamp(t);
    return t;
  }();
  return d;
}

// 当前生效的主题指针。启动即指向默认主题 —— 所以任何时刻读 g_theme 都有效,
// 不需要"必须先 load 才能用"的顺序约定。
const Theme* g_theme_ptr = &theme_defaults();

// 从 flash 主题分区加载后的落地缓冲。
// 用 .bss 而不是直接指向 flash 里的 JSON:JSON 不是 Theme 的内存布局,
// 必须先解析;解析结果总得有地方放。312 字节的 .bss 远比 36KB 的
// LVGL 内部增长划算(见 ui_theme.h 的说明)。
static Theme s_loaded;

void theme_loaded_slot(Theme** out) {
  *out = &s_loaded;
}

void theme_use_loaded() {
  g_theme_ptr = &s_loaded;
}

// 启动时确保主题可用。
//
// ★ 顺序陷阱(踩过一次,表现为"日志说已加载、画面却是默认配色"):
//   调用方的典型顺序是
//       theme_load();      // 尝试从 flash 主题分区加载
//       dash_ui_init();    // 内部会调本函数
//   所以这里**不能**无条件重置指针,否则刚加载的主题会被立刻丢掉。
//   只在确实没有可用主题时才落到默认值。
void theme_reset_to_defaults() {
  if (g_theme_ptr == nullptr) {
    g_theme_ptr = &theme_defaults();
  }
}

// 无条件回到默认主题(留给"恢复出厂设置"这类显式操作)
void theme_force_defaults() {
  g_theme_ptr = &theme_defaults();
}

void theme_clamp(Theme& t) {
  // LVGL 对负半径/超大量程不会报错,只会画出鬼东西或者卡死,所以必须挡。
  if (t.face_size < 40)  t.face_size = 40;
  if (t.face_size > 480) t.face_size = 480;
  if (t.coolant_max_c <= t.coolant_min_c) {
    t.coolant_min_c = 60.0f;
    t.coolant_max_c = 130.0f;
  }
  if (t.intake_max_c <= t.intake_min_c) {
    t.intake_min_c = 0.0f;
    t.intake_max_c = 80.0f;
  }
  // 开机动画时长在 boot_anim.cpp 里会当除数用,0 会直接除零崩掉。
  if (t.boot_fade_ms == 0)        t.boot_fade_ms = 1;
  if (t.boot_sweep_rise_ms == 0)  t.boot_sweep_rise_ms = 1;
  if (t.boot_sweep_fall_ms == 0)  t.boot_sweep_fall_ms = 1;
  if (t.boot_face_blink_ms == 0)  t.boot_face_blink_ms = 130;
  if (t.boot_face_start_ms == 0)  t.boot_face_start_ms = 1000;

  // 派生字段:总时长 = 表情开始 + 一拍收尾。
  // 必须在上面把这两个值钳到非零之后算。
  // ★ 以前是 + 4*blink(眨眼两轮),眨眼状态删掉后就只剩"表情出现"这一个动作,
  //   4 拍纯属白等 —— 开机期间 dash_ui_render 会早退,那段时间根本不显示真实数据。
  t.boot_total_ms = t.boot_face_start_ms + t.boot_face_blink_ms;

  for (uint8_t s = 0; s < 2; ++s) {
    ScreenTheme& sc = t.screens[s];
    if (sc.arc_count > kMaxArcs) sc.arc_count = kMaxArcs;
    sc.show_face = sc.show_face ? 1 : 0;
    for (uint8_t i = 0; i < sc.arc_count; ++i) {
      ArcStyle& a = sc.arcs[i];
      if ((uint8_t)a.kind > (uint8_t)ArcKind::Intake) a.kind = ArcKind::Speed;
      if (a.radius < 10)  a.radius = 10;
      if (a.radius > 240) a.radius = 240;
      if (a.width < 1)    a.width = 1;
      if (a.width > 60)   a.width = 60;
      if (a.end_deg <= a.start_deg) a.end_deg = a.start_deg + 1;
      a.reverse = a.reverse ? 1 : 0;
    }
  }

  // 数字读数:钳住字号编号与位置。
  // 位置越界不会崩,但会把数字画到屏外或糊在弧上 —— 所以挡在合理区间内。
  if (t.readout.digit_font > 1) t.readout.digit_font = 0;
  if (t.readout.unit_font  > 1) t.readout.unit_font  = 1;
  // 竖直位置:0..480 之内;数字中心别低到 120 以下(那里是表情区)
  if (t.readout.digit_cy < 10)  t.readout.digit_cy = 10;
  if (t.readout.digit_cy > 115) t.readout.digit_cy = 115;
  if (t.readout.unit_cy  < 10)  t.readout.unit_cy  = 10;
  if (t.readout.unit_cy  > 119) t.readout.unit_cy  = 119;
  if (t.readout.coolant_cy < 200) t.readout.coolant_cy = 200;
  if (t.readout.coolant_cy > 470) t.readout.coolant_cy = 470;
  if (t.readout.intake_cy  < 200) t.readout.intake_cy  = 200;
  if (t.readout.intake_cy  > 470) t.readout.intake_cy  = 470;
  t.readout.show_units   = t.readout.show_units ? 1 : 0;
  t.readout.show_coolant = t.readout.show_coolant ? 1 : 0;
  t.readout.show_intake  = t.readout.show_intake ? 1 : 0;
}

// theme_after_load 已并入 theme_parse_json(它直接对传进来的对象调 theme_clamp)。
// 保留这个空实现只为兼容旧调用点,新代码不要再依赖它。
void theme_after_load() {
  theme_clamp(s_loaded);
}

// ============================================================
// 视图映射与预览注入的用例（2026-09-24）
//
// 这一组测的是**没有 LVGL、也没有硬件**的那两层：
//   ① src/ui_model.h 的指示灯槽位：几何约束（内切圆 / 不压小字）与
//      "哪一格该亮"（六格 → 车状态 + 告警的映射）；
//   ② src/preview_input.h 的注入语义：键位、控制文件语法、覆盖关系。
//
// ★ 为什么这两件事值得单独一组用例：
//   几何算错**不会报错** —— 只会让灯条在圆屏上被削掉一角、或者压住水温数字，
//   而"看着不对劲"是最难自查的一类问题（仓库里 test-gauge-geometry.js 的
//   文件头就是为同一类事写的）。注入那一层则是"模拟页能不能复现某个状态"
//   的唯一依据：语义错了，报告里贴的控制文件就复现不出那张图。
//
// ★ 本文件是**唯一**允许 include `src/` 的 native 用例（平台侧为了让
//   这两层可测，给 [env:native] 加了 `-I src`，见 platformio.ini 那段注释）。
//   `src/` 里那些 .cpp **不参与** native 链接，所以这里只能碰纯头文件里的
//   inline 函数与纯逻辑 —— 一碰 LVGL/Arduino 符号就会链接失败（这正是想要的闸门）。
// ============================================================
#include <unity.h>
#include <string.h>
#include <math.h>

#include "lamp_view.h"       // LampSlot / LampView / make_lamps + 灯条几何常量
#include "preview_input.h"   // preview_apply_key / preview_apply_control_text

// 480 基准、圆心 (240,240)、半径 240（内切圆）
static double distFromCenter(int32_t x, int32_t y) {
  const double dx = (double)x - 240.0;
  const double dy = (double)y - 240.0;
  return sqrt(dx * dx + dy * dy);
}

// ============================================================
// 一、灯条几何:六格都落在内切圆内（圆屏上不会被削掉）
// ============================================================
void test_lamp_row_fits_inscribed_circle(void) {
  // 先钉住那一组派生常量（它们是从 kLampSize/kLampGap 算出来的，
  // 改了任何一个都要重新核圆约束 —— 这里是那条"改了就跑"的闸门）
  TEST_ASSERT_EQUAL_INT32(kLampSlotCount * kLampSize + (kLampSlotCount - 1) * kLampGap,
                          kLampRowW);
  TEST_ASSERT_EQUAL_INT32(270, kLampRowW);
  TEST_ASSERT_EQUAL_INT32((480 - kLampRowW) / 2, kLampX0);
  TEST_ASSERT_EQUAL_INT32(105, kLampX0);

  for (uint8_t i = 0; i < kLampSlotCount; ++i) {
    const int32_t x0 = lampLeft((LampSlot)i);
    const int32_t y0 = lampTop((LampSlot)i);
    const int32_t x1 = x0 + kLampSize;
    const int32_t y1 = y0 + kLampSize;
    // 四角都要在圆内。★ 用 `< 240`（不是 `<=`）:圆屏上"角正好压在边线上"
    // 会被圆角/边框吃掉一点点，留一点余量是刻意的。
    const int32_t xs[2] = {x0, x1};
    const int32_t ys[2] = {y0, y1};
    for (int a = 0; a < 2; ++a) {
      for (int b = 0; b < 2; ++b) {
        const double d = distFromCenter(xs[a], ys[b]);
        TEST_ASSERT_TRUE(d < 240.0);
      }
    }
  }
}

// ============================================================
// 二、灯条不压副表小字（水温 / 进气数字）
// ============================================================
// ★ 期望值口径（与 ui_theme.h 的 480 基准一致，且**写死在这里是有意的**）:
//   副表数字中心 cy = 384、18 号字墨迹高约 13 ⇒ 墨迹 y ∈ [377.5, 390.5]。
//   （那一行"实测墨迹"出自 ui_theme.h 的注释：480 上单位字高 13、
//     cy=107 → 104..117，同理 384 → 377.5..390.5。）
//   灯条顶边必须在这之下 —— 差 1 像素也会在真屏上看着"字压着灯"。
void test_lamp_row_clears_aux_readout(void) {
  const int32_t readout_cy480 = 384;
  const int32_t unit_ink_h480 = 13;                  // 18 号墨迹高（实测值）
  const int32_t readout_bottom = readout_cy480 + (unit_ink_h480 + 1) / 2;

  const int32_t lamp_top = lampTop(LampSlot::LeftArrow);
  TEST_ASSERT_EQUAL_INT32(lamp_top, lampTop(LampSlot::Door));   // 六格同一行
  TEST_ASSERT_TRUE(lamp_top >= readout_bottom);
  // 顺手把"差多少"钉下来:以后调 kLampCy 时看得见余量还剩多少
  TEST_ASSERT_TRUE(lamp_top - readout_bottom >= 2);

  // 另一头:灯条底边也要留在屏内且不贴边
  const int32_t lamp_bottom = lamp_top + kLampSize;
  TEST_ASSERT_TRUE(lamp_bottom <= 480 - 20);
}

// ============================================================
// 三、六格 → 车状态 的映射（稳态灯不闪、转向灯闪、门提示、双闪）
// ============================================================
void test_lamp_view_maps_vehicle_state(void) {
  VehicleState s{};
  // 半周期的极性（与 lamp_view.h 的约定一起钉住）:
  //   [0, 375) 与 [750, 1125) = **亮**的那半；[375, 750) = 灭的那半。
  //   ★ 刻意从 0 起算:打灯那一刻(now=0)就该亮 —— 不然"刚打灯先黑 375 ms"
  //     看着像没反应（第一版就是这个极性反了，用例当场红）。
  const uint32_t on_ms  = 0;
  const uint32_t off_ms = kLampBlinkHalfMs;

  // ---- 稳态灯（灯杆控制的）:与时间无关，一直亮 ----
  s.low_beam = true;
  s.position_lamp = true;
  for (uint32_t t = 0; t < 4 * kLampBlinkHalfMs; t += kLampBlinkHalfMs) {
    const LampView v = make_lamps(s, kAlertNone, t, false);
    TEST_ASSERT_TRUE(v.on[(uint8_t)LampSlot::LowBeam]);
    TEST_ASSERT_TRUE(v.on[(uint8_t)LampSlot::PositionLamp]);
    TEST_ASSERT_EQUAL_UINT8(255, v.pulse[(uint8_t)LampSlot::LowBeam]);
    TEST_ASSERT_FALSE(v.alert[(uint8_t)LampSlot::LowBeam]);
  }
  s.low_beam = false;
  s.position_lamp = false;

  // ---- 转向灯:亮着的时候在闪（两个半周期一明一灭）----
  s.indicator_left = true;
  {
    const LampView a = make_lamps(s, kAlertNone, on_ms, false);
    const LampView b = make_lamps(s, kAlertNone, off_ms, false);
    TEST_ASSERT_TRUE(a.on[(uint8_t)LampSlot::LeftArrow]);
    TEST_ASSERT_FALSE(b.on[(uint8_t)LampSlot::LeftArrow]);
    TEST_ASSERT_EQUAL_UINT8(255, a.pulse[(uint8_t)LampSlot::LeftArrow]);
    TEST_ASSERT_EQUAL_UINT8(0, b.pulse[(uint8_t)LampSlot::LeftArrow]);
    // 右箭头不受影响（左右是两格，不是一格）
    TEST_ASSERT_FALSE(a.on[(uint8_t)LampSlot::RightArrow]);
    // 一个完整周期之后回到"亮"：闪是**周期**行为，不是一次性
    const LampView c = make_lamps(s, kAlertNone, 2 * kLampBlinkHalfMs, false);
    TEST_ASSERT_TRUE(c.on[(uint8_t)LampSlot::LeftArrow]);
  }

  // ---- 双闪:两位都亮 + 双闪那一格也亮 ----
  s.indicator_right = true;
  s.hazard = true;
  {
    const LampView v = make_lamps(s, kAlertNone, on_ms, false);
    TEST_ASSERT_TRUE(v.on[(uint8_t)LampSlot::LeftArrow]);
    TEST_ASSERT_TRUE(v.on[(uint8_t)LampSlot::RightArrow]);
    TEST_ASSERT_TRUE(v.on[(uint8_t)LampSlot::Hazard]);
  }

  // ---- 门提示:只有"动过"才亮（不是"门开着"—— 那个还没解出来）----
  s = VehicleState{};
  s.door_activity = true;
  {
    const LampView v = make_lamps(s, kAlertNone, on_ms, false);
    TEST_ASSERT_TRUE(v.on[(uint8_t)LampSlot::Door]);
    TEST_ASSERT_FALSE(v.on[(uint8_t)LampSlot::LeftArrow]);
  }
  s.door_activity = false;
  {
    const LampView v = make_lamps(s, kAlertNone, on_ms, false);
    TEST_ASSERT_FALSE(v.on[(uint8_t)LampSlot::Door]);
  }
}

// ============================================================
// 四、告警叠加:正在报的那一条要盖过它自己的节奏，且**只在脉冲拍上亮**
// ============================================================
void test_lamp_view_alert_overlay(void) {
  VehicleState s{};   // 车本身什么状态都没有 —— 只靠告警点灯
  // 转向灯那种"闪烁"的半周期时刻（见上一个用例里的约定）
  const uint32_t on_ms = 0;

  // ---- 超速：借左右两格，跟随告警脉冲 ----
  {
    const LampView a = make_lamps(s, kAlertOverspeed, 1000, true);
    TEST_ASSERT_TRUE(a.on[(uint8_t)LampSlot::LeftArrow]);
    TEST_ASSERT_TRUE(a.on[(uint8_t)LampSlot::RightArrow]);
    TEST_ASSERT_TRUE(a.alert[(uint8_t)LampSlot::LeftArrow]);
    TEST_ASSERT_TRUE(a.alert[(uint8_t)LampSlot::RightArrow]);

    const LampView b = make_lamps(s, kAlertOverspeed, 1000, false);
    TEST_ASSERT_FALSE(b.on[(uint8_t)LampSlot::LeftArrow]);   // 脉冲的另一拍:不亮
    TEST_ASSERT_FALSE(b.alert[(uint8_t)LampSlot::LeftArrow]);
  }

  // ---- 红区：借"仪表盘灯"那一格（超速/红区没有专属槽位 = 占位的意思）----
  {
    const LampView v = make_lamps(s, kAlertRedline, 1000, true);
    TEST_ASSERT_TRUE(v.on[(uint8_t)LampSlot::PositionLamp]);
    TEST_ASSERT_TRUE(v.alert[(uint8_t)LampSlot::PositionLamp]);
    TEST_ASSERT_FALSE(v.on[(uint8_t)LampSlot::LeftArrow]);
  }

  // ---- 转向灯忘关：对应那一格在闪，**加描边**（它本来就亮）----
  s.indicator_left = true;
  {
    const LampView v = make_lamps(s, kAlertTurnSig, on_ms, false);
    TEST_ASSERT_TRUE(v.alert[(uint8_t)LampSlot::LeftArrow]);
    TEST_ASSERT_FALSE(v.alert[(uint8_t)LampSlot::RightArrow]);
  }

  // ---- 门：门那一格本身亮 + 描边 ----
  s = VehicleState{};
  s.door_activity = true;
  {
    const LampView v = make_lamps(s, kAlertDoor, on_ms, false);
    TEST_ASSERT_TRUE(v.on[(uint8_t)LampSlot::Door]);
    TEST_ASSERT_TRUE(v.alert[(uint8_t)LampSlot::Door]);
  }

  // ---- 没有告警时:一根描边都不许有（描边 = "这一条正在报"的唯一视觉标记）----
  s = VehicleState{};
  s.low_beam = true;
  s.door_activity = true;
  {
    const LampView v = make_lamps(s, kAlertNone, on_ms, false);
    for (uint8_t i = 0; i < kLampSlotCount; ++i) TEST_ASSERT_FALSE(v.alert[i]);
  }
}

// ============================================================
// 五、注入:键位（每个键一指，X/Esc 全清）
// ============================================================
void test_preview_keys(void) {
  PreviewInput in;
  TEST_ASSERT_FALSE(in.any());

  // 转向灯是**开关**（按一下开、再按一下关）
  TEST_ASSERT_TRUE(preview_apply_key(in, PreviewKey::Left));
  TEST_ASSERT_TRUE(in.left && in.left_set);
  preview_apply_key(in, PreviewKey::Left);
  TEST_ASSERT_FALSE(in.left);
  TEST_ASSERT_TRUE(in.left_set);          // 设过就一直是"设过"（关也是一个选择）

  preview_apply_key(in, PreviewKey::Right);
  preview_apply_key(in, PreviewKey::Hazard);
  preview_apply_key(in, PreviewKey::LowBeam);
  preview_apply_key(in, PreviewKey::PositionLamp);
  preview_apply_key(in, PreviewKey::Door);
  TEST_ASSERT_TRUE(in.right && in.hazard && in.low_beam && in.position && in.door);

  // 超速/红区注入的是**具体数值**（阈值 + 余量），不是布尔
  preview_apply_key(in, PreviewKey::Overspeed);
  TEST_ASSERT_TRUE(in.speed_set);
  TEST_ASSERT_TRUE(in.speed_kmh > 120.0f);     // 高于 AlertsConfig 的默认超速阈值
  preview_apply_key(in, PreviewKey::Redline);
  TEST_ASSERT_TRUE(in.rpm_set);
  TEST_ASSERT_TRUE(in.rpm > 5800.0f);          // 高于红区阈值

  // 静音是开关
  TEST_ASSERT_FALSE(in.mute);
  preview_apply_key(in, PreviewKey::Mute);
  TEST_ASSERT_TRUE(in.mute);
  preview_apply_key(in, PreviewKey::Mute);
  TEST_ASSERT_FALSE(in.mute);

  // 全清:所有旗标都要回去（否则"按 X 之后还有一格亮着"永远查不出来）
  preview_apply_key(in, PreviewKey::Clear);
  TEST_ASSERT_FALSE(in.any());
  TEST_ASSERT_FALSE(in.mute);
  TEST_ASSERT_FALSE(in.speed_set && in.rpm_set);

  // 不认的键要**明确返回 false**（调用方据此打一行提示，别静默吞掉）
  TEST_ASSERT_FALSE(preview_apply_key(in, PreviewKey::None));
}

// ============================================================
// 六、注入:控制文件语法（键名、注释、空行、真假写法、不认的键）
// ============================================================
void test_preview_control_text(void) {
  PreviewInput in;
  // 一份"像报告里会贴的"控制文件
  const char* text =
      "# 打左灯 + 开门 + 超速\n"
      "left=1\n"
      "\n"
      "  door = 1  \n"
      "speed=140.5   # 单位 km/h\n"
      "hazard=0\n"
      "position=on\n"
      "low_beam=true\n"
      "mute=yes\n"
      "nonsense=1\n";

  // 认出来的 7 条：left / door / speed / hazard / position / low_beam / mute
  //   ★ 一开始写的是 8（把注释行与空行也算进去了）—— 数一遍就知道不对，
  //     而这条断言的价值恰恰在于"数字必须数清楚"（`nonsense=1` 不算）。
  const int n = preview_apply_control_text(in, text);
  TEST_ASSERT_EQUAL_INT(7, n);
  TEST_ASSERT_TRUE(in.left && in.left_set);
  TEST_ASSERT_TRUE(in.door && in.door_set);
  TEST_ASSERT_TRUE(in.speed_set);
  TEST_ASSERT_EQUAL_FLOAT(140.5f, in.speed_kmh);
  TEST_ASSERT_TRUE(in.hazard_set);
  TEST_ASSERT_FALSE(in.hazard);         // hazard=0 也要"设过"
  TEST_ASSERT_TRUE(in.position && in.position_set);   // on
  TEST_ASSERT_TRUE(in.low_beam && in.low_beam_set);   // true
  TEST_ASSERT_TRUE(in.mute);                          // yes

  // 认不出的键**不许**把别的键带歪（也不许报错崩掉）
  PreviewInput only_bad;
  TEST_ASSERT_EQUAL_INT(0, preview_apply_control_text(only_bad, "what=1\n# 全注释\n"));
  TEST_ASSERT_FALSE(only_bad.any());

  // 空文本 / 空指针是安全的（"没有控制文件"就是这一条）
  PreviewInput e;
  TEST_ASSERT_EQUAL_INT(0, preview_apply_control_text(e, ""));
  TEST_ASSERT_EQUAL_INT(0, preview_apply_control_text(e, nullptr));

  // clear=1 要把整份注入清掉
  PreviewInput c;
  preview_apply_control_text(c, "left=1 right=1 door=1 rpm=6000\n");
  TEST_ASSERT_TRUE(c.any());
  TEST_ASSERT_EQUAL_INT(1, preview_apply_control_text(c, "clear=1\n"));
  TEST_ASSERT_FALSE(c.any());
}

void register_ui_lamp_tests(void) {
  RUN_TEST(test_lamp_row_fits_inscribed_circle);
  RUN_TEST(test_lamp_row_clears_aux_readout);
  RUN_TEST(test_lamp_view_maps_vehicle_state);
  RUN_TEST(test_lamp_view_alert_overlay);
  RUN_TEST(test_preview_keys);
  RUN_TEST(test_preview_control_text);
}

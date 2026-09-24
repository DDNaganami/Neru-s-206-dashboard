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
#include "panel_view.h"      // 圆屏可视区（2.8C 档）的几何与遮罩着色规则

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

// ============================================================
// 七、遮罩开关（V 键 / `mask=`）：默认开、翻转、clear 回到"开"
// ============================================================
void test_preview_mask_toggle(void) {
  PreviewInput in;
  // 默认就是"开"——2.8C 档落的第一张图就该带可视圈标注
  TEST_ASSERT_TRUE(in.panel_mask);
  TEST_ASSERT_FALSE(in.panel_mask_set);      // 但"没显式设过"

  TEST_ASSERT_TRUE(preview_apply_key(in, PreviewKey::ToggleMask));
  TEST_ASSERT_FALSE(in.panel_mask);
  TEST_ASSERT_TRUE(in.panel_mask_set);       // 显式选择过 ⇒ 控制文件不许盖回去
  preview_apply_key(in, PreviewKey::ToggleMask);
  TEST_ASSERT_TRUE(in.panel_mask);

  // ★ 只按 V **不该**让 any() 变成 true：遮罩不是车辆状态，
  //   否则 `inject:` 那行回执会假装"车状态变了"。
  PreviewInput only_mask;
  preview_apply_key(only_mask, PreviewKey::ToggleMask);
  TEST_ASSERT_FALSE(only_mask.any());
  TEST_ASSERT_FALSE(only_mask.panel_mask);

  // 控制文件：mask=0 / mask=1 都算"设过"
  PreviewInput f;
  TEST_ASSERT_EQUAL_INT(1, preview_apply_control_text(f, "mask=0\n"));
  TEST_ASSERT_FALSE(f.panel_mask);
  TEST_ASSERT_TRUE(f.panel_mask_set);
  TEST_ASSERT_FALSE(f.any());
  TEST_ASSERT_EQUAL_INT(1, preview_apply_control_text(f, "mask=1\n"));
  TEST_ASSERT_TRUE(f.panel_mask);

  // clear=1 把遮罩复位成默认的"开"（"回到假数据的样子"= 带标注的样子）
  PreviewInput c;
  preview_apply_control_text(c, "mask=0 left=1\n");
  TEST_ASSERT_FALSE(c.panel_mask);
  TEST_ASSERT_EQUAL_INT(1, preview_apply_control_text(c, "clear=1\n"));
  TEST_ASSERT_TRUE(c.panel_mask);
  TEST_ASSERT_FALSE(c.panel_mask_set);
}

// ============================================================
// 八、注入 → 快照：双闪与左右箭头的合并语义
//   （这一条是"实测踩到"才加的：控制文件里写了 left=1，屏上却一直是
//     双闪的样子 —— 因为上一次注入的 hazard=1 把左右箭头也置位了，
//     而后来那次只写 left/right 时没人去清另外一位。）
// ============================================================
void test_preview_apply_snapshot(void) {
  VehicleState st{};
  TEST_ASSERT_FALSE(st.indicator_left || st.indicator_right || st.hazard);
  TEST_ASSERT_FALSE(st.low_beam || st.position_lamp || st.door_activity);

  // ---- ① 没注入过的通道一个都不许动 ----
  {
    PreviewInput none;
    VehicleState s{};
    s.speed_kmh = 42.0f;
    s.coolant_c = 88.0f;
    preview_apply_snapshot(none, s);
    TEST_ASSERT_EQUAL_FLOAT(42.0f, s.speed_kmh);
    TEST_ASSERT_EQUAL_FLOAT(88.0f, s.coolant_c);
    TEST_ASSERT_FALSE(s.indicator_left || s.indicator_right || s.hazard);
  }

  // ---- ② hazard=1：双闪 + 左右箭头一起（数据层的位域语义）----
  {
    PreviewInput in;
    preview_apply_control_text(in, "hazard=1\n");
    preview_apply_snapshot(in, st);
    TEST_ASSERT_TRUE(st.hazard);
    TEST_ASSERT_TRUE(st.indicator_left);
    TEST_ASSERT_TRUE(st.indicator_right);
  }

  // ---- ③ hazard=0：三位一起清（★ 这一条就是踩到的那个）----
  {
    PreviewInput in;
    preview_apply_control_text(in, "hazard=0\n");
    preview_apply_snapshot(in, st);
    TEST_ASSERT_FALSE(st.hazard);
    TEST_ASSERT_FALSE(st.indicator_left);
    TEST_ASSERT_FALSE(st.indicator_right);
  }

  // ---- ④ 同一帧里 `hazard=1 left=0 right=0` 也要表达得出来 ----
  //   （这就是"双闪先施加、箭头后施加"那个顺序换来的表达力）
  //   ★ 控制文件是**一行一条**：三个键要写三行（写成一行里的三个空格分隔项
  //     时，第二项会被当成第一项的"值" —— 实测踩到，parse 只认了 1 条）。
  {
    PreviewInput in;
    const int parsed = preview_apply_control_text(in, "hazard=1\nleft=0\nright=0\n");
    TEST_ASSERT_EQUAL_INT(3, parsed);
    TEST_ASSERT_TRUE(in.hazard_set);
    TEST_ASSERT_TRUE(in.left_set);
    TEST_ASSERT_TRUE(in.right_set);
    TEST_ASSERT_TRUE(in.hazard);
    TEST_ASSERT_FALSE(in.left);
    TEST_ASSERT_FALSE(in.right);
    VehicleState s{};                     // 干净状态：这一条验的是"合并语义"本身
    preview_apply_snapshot(in, s);
    TEST_ASSERT_TRUE(s.hazard);
    TEST_ASSERT_FALSE(s.indicator_left);
    TEST_ASSERT_FALSE(s.indicator_right);
    // 再施加一遍还是同一结果（幂等：控制文件是每帧重读的）
    preview_apply_snapshot(in, s);
    TEST_ASSERT_TRUE(s.hazard);
    TEST_ASSERT_FALSE(s.indicator_left);
  }

  // ---- ⑤ 单打一个左转：hazard 只按"注入写过没有"走（逐通道语义）----
  {
    PreviewInput in;
    preview_apply_control_text(in, "hazard=0\nleft=1\n");
    VehicleState s{};                     // ★ 每一段都用干净状态：注入只覆写
    preview_apply_snapshot(in, s);        //   它**写过**的那几位（这一条本身就是
    TEST_ASSERT_FALSE(s.hazard);          //   语义的一部分，别拿上一段的残留当输入）
    TEST_ASSERT_TRUE(s.indicator_left);
    TEST_ASSERT_FALSE(s.indicator_right);
    // 而"上一次注入留下的值"要能延续：同一份注入连续施加两次结果必须一样
    VehicleState s2{};
    preview_apply_snapshot(in, s2);
    TEST_ASSERT_EQUAL_INT(s.indicator_left, s2.indicator_left);
    TEST_ASSERT_EQUAL_INT(s.hazard, s2.hazard);
  }

  // ---- ⑥ 其余通道：灯位 / 门 / speed / rpm ----
  //   ★ 一行一条（控制文件的语法就是"每行一个键=值"；写成一行里的多个
  //     空格分隔项时，第二个开始会被当成第一个的"值" —— 实测 parse 只认 1 条）
  {
    PreviewInput in;
    preview_apply_control_text(in,
        "low_beam=1\nposition=1\ndoor=1\nspeed=130\nrpm=6000\n");
    VehicleState s{};
    preview_apply_snapshot(in, s);
    TEST_ASSERT_TRUE(s.low_beam);
    TEST_ASSERT_TRUE(s.position_lamp);
    TEST_ASSERT_TRUE(s.door_activity);
    TEST_ASSERT_EQUAL_FLOAT(130.0f, s.speed_kmh);
    TEST_ASSERT_EQUAL_FLOAT(6000.0f, s.rpm);
  }

  // ---- ⑦ 清过的注入不许再改快照（`clear=1` 之后是"没注入过"）----
  {
    PreviewInput in;
    preview_apply_control_text(in, "left=1\nspeed=200\nclear=1\n");
    VehicleState s{};
    s.speed_kmh = 55.0f;
    preview_apply_snapshot(in, s);
    TEST_ASSERT_FALSE(s.indicator_left);
    TEST_ASSERT_EQUAL_FLOAT(55.0f, s.speed_kmh);
  }
}

// ============================================================
// 九、「2.8C（最终板）」的圆屏几何与遮罩着色规则
//
// 这一组的价值：遮罩算歪**不会报错** —— 只会让"圆外压暗"那块形状不对，
// 而那正是"素材会不会被圆边切掉"的唯一参照。所以逐条钉住：
//   ① 内切正方形 = 336 / 168（与 tools/theme-editor/asset-spec.js 的
//      circleSafeSide() 同一套算式 —— 两边不一致时这一条会红）；
//   ② 有效区 Ø70.13mm、1 像素 0.1461mm 的换算；
//   ③ 着色规则三段（圆内不动 / 圈带半边暗 / 四角打点）；
//   ④ 遮罩**一个圆内像素都不许改**（那是真机画面）。
// ============================================================
void test_panel_round_geometry(void) {
  // ---- ① 内切正方形（两条链同一个数）----
  TEST_ASSERT_EQUAL_INT32(336, panelInscribedSquareSide(480));
  TEST_ASSERT_EQUAL_INT32(168, panelInscribedSquareSide(240));
  // 它必须是 **4 的倍数**（素材尺寸对齐口径，网页那边也是这条）
  TEST_ASSERT_EQUAL_INT32(0, panelInscribedSquareSide(480) % 4);
  TEST_ASSERT_EQUAL_INT32(0, panelInscribedSquareSide(240) % 4);
  // 而它必须真的落在圆里：对角线一半 <= 半径
  {
    const double half = panelInscribedSquareSide(480) * sqrt(2.0) / 2.0;
    TEST_ASSERT_TRUE(half <= panelRadiusPx(480) + 1e-6);
  }

  // ---- ② 物理口径：Ø70.13mm ⇒ 0.1461 mm/px ----
  TEST_ASSERT_EQUAL_INT32(7013, kPanelActiveAreaMm10);          // 70.13 mm（单位 1e-2 mm）
  TEST_ASSERT_EQUAL_INT32(1461, panelMmPerPx10000(480));        // 0.1461 mm/px（单位 1e-4 mm）
  // 480 个像素整好铺满有效区（换算与直径自洽）
  TEST_ASSERT_EQUAL_INT32(7013, panelPxToMm100(480, 480));      // = 70.13 mm
  TEST_ASSERT_EQUAL_INT32(3506, panelPxToMm100(240, 480));      // 半径 ≈ 35.06 mm

  // ---- ③ 圆内 / 圆外：四个角一定在圆外，屏心一定在圆内 ----
  TEST_ASSERT_TRUE(panelPixelVisible(240, 240, 480));
  TEST_ASSERT_TRUE(panelPixelVisible(240, 0, 480));     // 上边中点（在圆上）
  TEST_ASSERT_TRUE(panelPixelVisible(0, 240, 480));     // 左边中点
  TEST_ASSERT_FALSE(panelPixelVisible(0, 0, 480));      // 四角
  TEST_ASSERT_FALSE(panelPixelVisible(479, 479, 480));
  TEST_ASSERT_FALSE(panelPixelVisible(0, 479, 480));
  TEST_ASSERT_FALSE(panelPixelVisible(479, 0, 480));
  // 圆外像素占比应当明显小于一半（四角那四块 ≈ 21%）
  {
    const int32_t outside = panelOutsidePixelCount(480);
    const int32_t total = 480 * 480;
    TEST_ASSERT_TRUE(outside > 0);
    TEST_ASSERT_TRUE(outside < total / 4);
  }

  // ---- ④ 着色规则三段 ----
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeInside, panelShadeAt(240, 240, 480));
  // ★ 边界这一条挑的是**圆外头几像素**那一圈：圆内最边上一列/一行是
  //   (0,225) / (240,0)（距离 239.94 / 239.5，还在圆里），往斜下走两像素
  //   就出圆了 —— (0,224) 的距离正好 240.00，落在参考圈带里。
  //   （第一版把坐标写成 (0,240)/(240,1)，那两点其实**在圆内** ⇒ 用例当场红。
  //    这几个点是用 `sqrt(240² - (x+0.5-240)²)` 算出来的，不是估的。）
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeInside, panelShadeAt(0, 225, 480));
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeInside, panelShadeAt(240, 0, 480));
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeRing, panelShadeAt(0, 224, 480));
  TEST_ASSERT_EQUAL_UINT16(kPanelShadeRing, panelShadeAt(1, 212, 480));
  {
    const uint16_t c1 = panelShadeAt(2, 2, 480);
    const uint16_t c2 = panelShadeAt(4, 2, 480);
    TEST_ASSERT_TRUE(c1 == kPanelShadeCornerA || c1 == kPanelShadeCornerB);
    TEST_ASSERT_TRUE(c2 == kPanelShadeCornerA || c2 == kPanelShadeCornerB);
    TEST_ASSERT_TRUE(c1 != c2);       // 棋盘格：相邻两格必须不一样（否则看不出是标注）
  }
  // 圈带宽度按分辨率缩（480 档 3 px，240 档也不小于 2）
  TEST_ASSERT_EQUAL_INT32(3, panelRingWidthPx(480));
  TEST_ASSERT_TRUE(panelRingWidthPx(240) >= 2);
}

void test_panel_overlay_leaves_inside_untouched(void) {
  // 造一份"每个像素都不一样"的缓冲（值 = 索引的低 16 位），跑完遮罩之后
  // **圆内必须逐像素等于原值**、圆外必须逐像素变了。这一条把
  // "遮罩会不会误伤真机可见区"钉死。
  static uint16_t buf[480 * 480];
  static uint16_t orig[480 * 480];
  for (int32_t i = 0; i < 480 * 480; ++i) {
    // ★ 三个通道**都非零**：R = 0x1F、B = 0x1F、G = (i & 0x3F) + 1。
    //   为什么刻意避开任何通道为 0：通道值 0 乘任何亮度还是 0，那样
    //   "圆外的像素被改过没有"会**假绿**（实测踩到：第一版用
    //   `0x0800 + (i & 0x7FF)`，于是 G 在 1/2 亮度下被 0x0800>>1 抹掉、
    //   看起来"没变"，用例报 7 个圆外像素未变 —— 数是对的，图案不对）。
    buf[i] = (uint16_t)(0x801Fu | ((uint16_t)((i & 0x3Fu) + 1u) << 5));
    orig[i] = buf[i];
  }
  const int32_t changed = panelApplyOverlayRgb565(buf, 480);
  TEST_ASSERT_EQUAL_INT32(panelOutsidePixelCount(480), changed);

  int32_t inside_changed = 0, outside_unchanged = 0;
  for (int32_t y = 0; y < 480; ++y) {
    for (int32_t x = 0; x < 480; ++x) {
      const int32_t i = y * 480 + x;
      if (panelPixelVisible(x, y, 480)) {
        if (buf[i] != orig[i]) ++inside_changed;
      } else {
        if (buf[i] == orig[i]) ++outside_unchanged;
      }
    }
  }
  TEST_ASSERT_EQUAL_INT32(0, inside_changed);
  TEST_ASSERT_EQUAL_INT32(0, outside_unchanged);

  // 缩放是"按通道"的（不是把 16 位整数整体乘）—— 否则半边暗会串色。
  // 圈带（k=128）下，一个纯红像素应当还是"红占优"。
  {
    uint16_t px = (uint16_t)(0x1Fu << 11);      // RGB565 纯红
    const uint16_t k = kPanelShadeRing;
    const uint16_t r = panelScaleRgb565Channel((uint16_t)((px >> 11) & 0x1Fu), k);
    const uint16_t g = panelScaleRgb565Channel((uint16_t)((px >> 5) & 0x3Fu), k);
    const uint16_t b = panelScaleRgb565Channel((uint16_t)(px & 0x1Fu), k);
    TEST_ASSERT_EQUAL_UINT16(16, r);      // 31 * 128/256 ≈ 15.5 → 16
    TEST_ASSERT_EQUAL_UINT16(0, g);
    TEST_ASSERT_EQUAL_UINT16(0, b);
  }
}

void register_ui_lamp_tests(void) {
  RUN_TEST(test_lamp_row_fits_inscribed_circle);
  RUN_TEST(test_lamp_row_clears_aux_readout);
  RUN_TEST(test_lamp_view_maps_vehicle_state);
  RUN_TEST(test_lamp_view_alert_overlay);
  RUN_TEST(test_preview_keys);
  RUN_TEST(test_preview_control_text);
  RUN_TEST(test_preview_mask_toggle);
  RUN_TEST(test_preview_apply_snapshot);
  RUN_TEST(test_panel_round_geometry);
  RUN_TEST(test_panel_overlay_leaves_inside_untouched);
}

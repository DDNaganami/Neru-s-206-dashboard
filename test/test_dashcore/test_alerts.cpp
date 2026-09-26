// ============================================================
// 告警层用例（2026-09-24）
//
// 三条规则的每一条都要有**正反两面**（这才叫测掉了）:
//   ① 去抖:短于窗口不报 / 连续够了才报 / 中途断掉要**重新计时**
//   ② 最短重复间隔:一直成立也只是"每 N 毫秒响一声",不会变成长鸣
//   ③ 静音:掐蜂鸣器、**不掐**屏上那条(active() 照旧)
// 外加:优先级、迟滞、只认已解字段(双闪不算"忘关")、reset 的语义、
//       以及蜂鸣器抽象本身（宿主实现真的落了一行、空实现真的什么都不做）。
//
// ★ 全部是**纯逻辑**用例:不碰 LVGL、不碰串口、不读时钟 —— Alerts::update()
//   的 now_ms 由用例自己喂,所以"20 秒"这种窗口在用例里是零成本的。
//
// ★★ 时间口径（本轮踩过一次,写下来免得下次再踩）:
//   本类用例一律用**绝对时刻**推进（`t = X; a.update(s, t)` 一个点一个点走），
//   **不用**"从某个起点累加 dt"的循环。原因:去抖是"从条件**首次成立**那一刻
//   起连续算"(见 Alerts::debounce),而累加式循环里"循环起点"与"条件首次成立
//   的时刻"不是一回事 —— 第一版就是这么写歪的,五条用例集体假红,
//   而实现本身是对的。绝对时刻写法的断言一眼能看出在验哪一刻。
// ============================================================
#include <unity.h>
#include <string.h>
#include "alerts.h"
#include "buzzer.h"

// 造一个干净的 VehicleState（默认值里 speed=0 / rpm=0，不会误触发）
static VehicleState clean() {
  VehicleState s{};
  s.speed_kmh = 0.0f;
  s.rpm = 0.0f;
  return s;
}

// ============================================================
// 一、什么都不满足 ⇒ 不报、不响
// ============================================================
void test_alerts_idle_by_default(void) {
  Alerts a;
  const VehicleState s = clean();
  for (uint32_t t = 1000; t <= 5000; t += 100) {
    TEST_ASSERT_TRUE(a.update(s, t) == AlertKind::None);
  }
  TEST_ASSERT_EQUAL_UINT32(0u, a.beepCount());
  TEST_ASSERT_FALSE(a.beeping());
  TEST_ASSERT_TRUE(a.pattern() == BeepPattern::Silent);

  // ★ 只为"未解字段不给告警"这条纪律留个反证:水温/进气/油量乱填到危险值,
  //   也**一条都不许触发**（它们要么是 OBD 的、要么根本没解出来）。
  VehicleState hot{};
  hot.coolant_c = 130.0f;
  hot.intake_c = 90.0f;
  hot.fuel_pct = 1.0f;
  TEST_ASSERT_TRUE(a.update(hot, 5100) == AlertKind::None);
  TEST_ASSERT_EQUAL_UINT32(0u, a.beepCount());
}

// ============================================================
// 二、超速:进 120 / 退 117(迟滞)
// ============================================================
void test_alerts_overspeed_with_hysteresis(void) {
  Alerts a;
  VehicleState s = clean();

  // 119.9 < 120:连跑 3 秒都不报（顺带证明"没到阈值就是没到,与时间无关"）
  s.speed_kmh = a.config().overspeed_kmh - 0.1f;
  for (uint32_t t = 1000; t <= 4000; t += 100) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  TEST_ASSERT_EQUAL_UINT32(0u, a.beepCount());

  // 到 120:去抖窗口 250 ms **差一点**不够 ⇒ 还不报（4500 - 4400 = 100）
  s.speed_kmh = a.config().overspeed_kmh;
  a.update(s, 4400);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  // 够 250 ms ⇒ 报,并**立刻**响一声（首声不等重复间隔）
  a.update(s, 4650);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Overspeed);
  TEST_ASSERT_TRUE(a.beeping());
  TEST_ASSERT_TRUE(a.pattern() == BeepPattern::Urgent);
  TEST_ASSERT_EQUAL_UINT32(1u, a.beepCount());

  // 掉到 118（> 117，在迟滞带里）⇒ **继续报**（不来回跳）
  s.speed_kmh = 118.0f;
  a.update(s, 4750);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Overspeed);

  // 掉到 116（< 117）⇒ 立刻清（恢复不需要去抖）
  s.speed_kmh = 116.0f;
  a.update(s, 4850);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  TEST_ASSERT_FALSE(a.beeping());

  // 回到 119（> 117 但 < 120）⇒ 仍然不报 —— 迟滞是**两侧**的
  s.speed_kmh = 119.0f;
  for (uint32_t t = 4950; t <= 7000; t += 100) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
}

// ============================================================
// 三、转速红区:5800 / 退 5600
// ============================================================
void test_alerts_redline(void) {
  Alerts a;
  VehicleState s = clean();

  s.rpm = a.config().redline_rpm - 10.0f;        // 5790 ⇒ 不报
  for (uint32_t t = 1000; t <= 3000; t += 100) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);

  s.rpm = a.config().redline_rpm;                // 5800 ⇒ 报三短
  a.update(s, 3100);
  a.update(s, 3400);                             // 300 ≥ 250 的去抖窗口
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
  TEST_ASSERT_TRUE(a.pattern() == BeepPattern::Triple);
  TEST_ASSERT_EQUAL_UINT32(1u, a.beepCount());

  // 退到 5700（> 5600 = 5800 - 200）⇒ 仍在迟滞带,继续报
  s.rpm = a.config().redline_rpm - a.config().redline_hyst_rpm + 100.0f;
  a.update(s, 3500);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);

  // 退到 5500 ⇒ 清
  s.rpm = a.config().redline_rpm - a.config().redline_hyst_rpm - 100.0f;
  a.update(s, 3600);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
}

// ============================================================
// 四、最短重复间隔:一直成立也只是"每 N 毫秒响一声"
// ============================================================
// ★ 这条就是"别让它一直叫"的直接证据:红区**一直**成立 15 秒,
//   响的次数必须是 1（首声）+ 每 3 秒一声 = 5 声,而不是"每一拍都响"。
void test_alerts_beep_repeat_interval(void) {
  Alerts a;
  VehicleState s = clean();
  s.rpm = 6000.0f;

  a.update(s, 1000);        // 条件首次成立 ⇒ 计时窗口从这一刻起
  a.update(s, 1100);        // 100 ms:还在去抖
  TEST_ASSERT_EQUAL_UINT32(0u, a.beepCount());
  a.update(s, 1300);        // 300 ms ≥ 250 ⇒ 第一声
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
  TEST_ASSERT_EQUAL_UINT32(1u, a.beepCount());

  // 到 4299 为止（< 1300 + 3000）一次都不许再响
  for (uint32_t t = 1400; t <= 4299; t += 100) a.update(s, t);
  TEST_ASSERT_EQUAL_UINT32(1u, a.beepCount());
  TEST_ASSERT_FALSE(a.beeping());

  // 越过 1300 + 3000 ⇒ 第二声
  a.update(s, 4300);
  TEST_ASSERT_EQUAL_UINT32(2u, a.beepCount());
  TEST_ASSERT_TRUE(a.beeping());

  // 一直成立到 16300 ⇒ 一共 6 声
  //   （首声 1300;之后每 3000 ms 一声:4300 / 7300 / 10300 / 13300 / 16300）
  //   算清楚再断言 —— 5 声是漏数了末尾那一拍（实现是对的,期望值写错了）。
  for (uint32_t t = 4400; t <= 16300; t += 100) a.update(s, t);
  TEST_ASSERT_EQUAL_UINT32(6u, a.beepCount());
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
}

// ============================================================
// 五、静音:掐蜂鸣器、不掐屏上那条
// ============================================================
void test_alerts_mute_switch(void) {
  Alerts a;
  VehicleState s = clean();
  s.rpm = 6000.0f;

  a.setMuted(true);
  TEST_ASSERT_TRUE(a.muted());
  for (uint32_t t = 1000; t <= 6000; t += 100) a.update(s, t);
  // 屏上照旧报（静音 ≠ 假装没事）,但一声都没响
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
  TEST_ASSERT_FALSE(a.beeping());
  TEST_ASSERT_TRUE(a.pattern() == BeepPattern::Silent);
  TEST_ASSERT_EQUAL_UINT32(0u, a.beepCount());

  // 取消静音 ⇒ **立刻**响（静音期间没响过,所以不必等重复间隔）
  a.setMuted(false);
  a.update(s, 6100);
  TEST_ASSERT_TRUE(a.beeping());
  TEST_ASSERT_EQUAL_UINT32(1u, a.beepCount());

  // toggle 也要能用（真机上就是一个键）
  a.toggleMuted();
  TEST_ASSERT_TRUE(a.muted());
  a.toggleMuted();
  TEST_ASSERT_FALSE(a.muted());

  // 静音之后再响,间隔仍然按"上一次**真响**的时刻"算
  a.setMuted(true);
  a.update(s, 7000);
  a.setMuted(false);
  a.update(s, 8000);        // 700 距上一次真响（6100）不到 3 秒 ⇒ 不响
  TEST_ASSERT_EQUAL_UINT32(1u, a.beepCount());
  a.update(s, 9200);        // 3100 ≥ 3000 ⇒ 响
  TEST_ASSERT_EQUAL_UINT32(2u, a.beepCount());
}

// ============================================================
// 六、门:用"动过",去抖 200 ms
// ============================================================
void test_alerts_door_uses_activity(void) {
  Alerts a;
  VehicleState s = clean();

  // 门信号"动过"只持续 100 ms（< 200 ms 去抖）⇒ 不报
  s.door_activity = true;
  a.update(s, 1000);
  a.update(s, 1100);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);

  // 断了 ⇒ 计时清零（下一次要从头来）
  s.door_activity = false;
  a.update(s, 1200);

  // 连续成立 300 ms ⇒ 报,给"一长"
  s.door_activity = true;
  a.update(s, 1300);
  a.update(s, 1450);                       // 150 < 200 ⇒ 还不报
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  a.update(s, 1600);                       // 300 ≥ 200 ⇒ 报
  TEST_ASSERT_TRUE(a.active() == AlertKind::Door);
  TEST_ASSERT_TRUE(a.pattern() == BeepPattern::Long);

  // 活动结束 ⇒ 立刻清
  s.door_activity = false;
  a.update(s, 1700);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
}

// ============================================================
// 七、转向灯忘关:连续 20 秒;中途灭了要**重新计时**;双闪不算
// ============================================================
void test_alerts_turn_signal_left_on(void) {
  const uint32_t kOn = 10000;       // 打灯那一刻
  const uint32_t kNeed = AlertsConfig{}.turn_signal_on_ms;

  // ---- ① 一直亮着:差一点不报、够 20 秒才报 ----
  Alerts a;
  VehicleState s = clean();
  s.indicator_left = true;
  for (uint32_t t = kOn; t <= kOn + kNeed - 100; t += 100) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);       // 19900:还没到
  a.update(s, kOn + kNeed);
  TEST_ASSERT_TRUE(a.active() == AlertKind::TurnSignal);
  TEST_ASSERT_TRUE(a.pattern() == BeepPattern::Short);
  TEST_ASSERT_EQUAL_UINT32(1u, a.beepCount());
  // 打回（左灭）⇒ 立刻清
  s.indicator_left = false;
  a.update(s, kOn + kNeed + 100);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);

  // ---- ② 中途灭一下 ⇒ 计时**从头开始**（不许累计）----
  // ★ 这一条同时说明**两处窗口必须配合**:真实转向灯每 0.8 秒就灭一次,
  //   数据层的 600 ms 保持窗口（kIndicatorHoldMs）把"亮着"这一段补平之后,
  //   `indicator_left` 才是连续的 true ⇒ 这个 20 秒计时器才攒得起来。
  //   若数据层不做保持窗口,这里会以 1.25 Hz 翻转 ⇒ 忘关告警形同不存在。
  Alerts b;
  VehicleState s2 = clean();
  s2.indicator_right = true;
  for (uint32_t t = kOn; t <= kOn + kNeed - 5000; t += 100) b.update(s2, t);
  s2.indicator_right = false;                            // 灭了 100 ms
  b.update(s2, kOn + kNeed - 4900);
  s2.indicator_right = true;                             // 再亮:从这一刻重新计时
  const uint32_t restart = kOn + kNeed - 4800;
  for (uint32_t t = restart; t <= restart + kNeed - 100; t += 100) b.update(s2, t);
  TEST_ASSERT_TRUE(b.active() == AlertKind::None);       // 累计早过 20 秒,但不许报
  b.update(s2, restart + kNeed);
  TEST_ASSERT_TRUE(b.active() == AlertKind::TurnSignal);
}

void test_alerts_hazard_is_not_forgotten_signal(void) {
  Alerts a;
  VehicleState s = clean();
  // 双闪:两位都置位 + hazard=true ⇒ 一直亮 60 秒都不报
  // （双闪是人**主动**按的,而且本来就允许长期亮 —— 比如临时停车）
  s.indicator_left = true;
  s.indicator_right = true;
  s.hazard = true;
  for (uint32_t t = 1000; t <= 61000; t += 250) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  TEST_ASSERT_EQUAL_UINT32(0u, a.beepCount());
}

// ============================================================
// 八、优先级:同时命中时只报最高那一条,且顺序稳定
// ============================================================
void test_alerts_priority_and_single_output(void) {
  Alerts a;
  VehicleState s = clean();
  const uint32_t kTurnOn = 1000;

  // 四条全命中:超速 130 + 红区 6000 + 门活动 + 左转亮着
  s.speed_kmh = 130.0f;
  s.rpm = 6000.0f;
  s.door_activity = true;
  s.indicator_left = true;
  s.hazard = false;

  // 走到"转向那条也够 20 秒"之后（此时四条都成立）
  const uint32_t tEnd = kTurnOn + a.config().turn_signal_on_ms + 1000u;
  for (uint32_t t = kTurnOn; t <= tEnd; t += 250) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Overspeed);       // 最高
  // ★ 这里断言的是 `patternFor()`,**不是** `a.pattern()`:
  //   `pattern()` 只在"这一拍正在响"时才有值（其余时候是 Silent,
  //   见 alerts.cpp 的最后一行）—— 循环最后一拍未必落在该响的那一刻。
  //   "模式映射"与"什么时候响"是两件事,别在一条断言里混。
  TEST_ASSERT_TRUE(Alerts::patternFor(a.active()) == BeepPattern::Urgent);

  // 车速掉下去 ⇒ 下一条顶上（红区），而且**模式跟着换**
  s.speed_kmh = 0.0f;
  a.update(s, tEnd + 250);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
  TEST_ASSERT_TRUE(Alerts::patternFor(a.active()) == BeepPattern::Triple);
  TEST_ASSERT_TRUE(a.beepCount() >= 2u);      // 换了一条 ⇒ 可以立刻响

  // 转速也掉 ⇒ 门顶上
  s.rpm = 0.0f;
  a.update(s, tEnd + 500);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Door);

  // 门也不动了 ⇒ 转向那条顶上（它一直在计时）
  s.door_activity = false;
  a.update(s, tEnd + 750);
  TEST_ASSERT_TRUE(a.active() == AlertKind::TurnSignal);

  // 全清 ⇒ 一条都没有了
  s.indicator_left = false;
  a.update(s, tEnd + 1000);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  TEST_ASSERT_TRUE(a.pattern() == BeepPattern::Silent);
}

// ============================================================
// 九、reset() 的语义:清状态机、**不清**静音与计数
// ============================================================
void test_alerts_reset_semantics(void) {
  Alerts a;
  VehicleState s = clean();
  s.rpm = 6000.0f;
  a.update(s, 1000);
  a.update(s, 1300);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
  const uint32_t beeps = a.beepCount();
  TEST_ASSERT_EQUAL_UINT32(1u, beeps);
  a.setMuted(true);

  a.reset();
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  TEST_ASSERT_TRUE(a.pattern() == BeepPattern::Silent);
  TEST_ASSERT_FALSE(a.beeping());
  TEST_ASSERT_EQUAL_UINT32(beeps, a.beepCount());   // 计数保留
  TEST_ASSERT_TRUE(a.muted());                      // 静音保留（是人的选择）

  // 复位之后条件仍成立 ⇒ 要**重新**去抖（不是"立刻又报"）
  a.setMuted(false);
  a.update(s, 5000);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);   // 计时窗口从这一刻重开
  a.update(s, 5200);                                 // 200 < 250 ⇒ 还不报
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  a.update(s, 5300);                                 // 300 ≥ 250 ⇒ 报
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
}

// ============================================================
// 十、蜂鸣器抽象本身:空实现真的什么都不做;宿主实现真的落一行
// ============================================================
// ★ 这一组是"抽象后面挂着可替换实现"这句话的**可验证形式**:
//   真机上要换的是另一个 Buzzer 子类（2.8C 的 TCA9554 EXIO8），
//   而 alerts 那一侧一行都不用动 —— 所以这里对两个实现的接口行为各钉一条。
void test_buzzer_abstraction(void) {
  const char* buzzer_host_last_line(void);   // 见 buzzer.cpp

  BuzzerNull none;
  none.begin();
  none.beep(BeepPattern::Urgent, 120);
  none.off();
  TEST_ASSERT_EQUAL_STRING("null", none.name());

  BuzzerHost host;
  host.begin();
  host.beep(BeepPattern::Triple, 120);
  host.off();
  TEST_ASSERT_EQUAL_STRING("host", host.name());
  // 格式固定为 "BEEP pattern=<名字> ms=<数>"（纯 ASCII,见 buzzer.cpp 的说明）
  TEST_ASSERT_EQUAL_STRING("BEEP pattern=triple ms=120", buzzer_host_last_line());

  // 五种模式的名字都要有（不许有空洞 —— 屏幕上/日志里会出现"?"）
  TEST_ASSERT_EQUAL_STRING("silent", beepPatternName(BeepPattern::Silent));
  TEST_ASSERT_EQUAL_STRING("short", beepPatternName(BeepPattern::Short));
  TEST_ASSERT_EQUAL_STRING("triple", beepPatternName(BeepPattern::Triple));
  TEST_ASSERT_EQUAL_STRING("urgent", beepPatternName(BeepPattern::Urgent));
  TEST_ASSERT_EQUAL_STRING("long", beepPatternName(BeepPattern::Long));

  // 告警名同理
  TEST_ASSERT_EQUAL_STRING("none", alertName(AlertKind::None));
  TEST_ASSERT_EQUAL_STRING("overspeed", alertName(AlertKind::Overspeed));
  TEST_ASSERT_EQUAL_STRING("redline", alertName(AlertKind::Redline));
  TEST_ASSERT_EQUAL_STRING("door", alertName(AlertKind::Door));
  TEST_ASSERT_EQUAL_STRING("turn-signal", alertName(AlertKind::TurnSignal));

  // 模式映射:每条告警一个模式,且**没有两条共用**（共用就分不出是谁在响）
  const BeepPattern pats[4] = {
    Alerts::patternFor(AlertKind::Overspeed), Alerts::patternFor(AlertKind::Redline),
    Alerts::patternFor(AlertKind::Door),      Alerts::patternFor(AlertKind::TurnSignal),
  };
  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_TRUE(pats[i] != BeepPattern::Silent);
    for (int j = i + 1; j < 4; ++j) TEST_ASSERT_TRUE(pats[i] != pats[j]);
  }
}

// ============================================================
// 四、配置钳制（2026-09-27 新增）
//
// 为什么要有它：这份配置现在**能从主题文件来**（编辑器里拖控件改，见
// lib/themetool/theme_store.cpp 的 theme_parse_alerts_json）⇒ 它从"编译期常量"
// 变成了"外部输入"。下面每一条都对应一个"手滑写进去、屏上和日志上都看不出来"
// 的坑 —— 所以每条都验**边界值本身**，而不是"随便钳一下"。
// ============================================================
void test_alerts_config_clamp_ranges(void) {
  AlertsConfig c;
  // 出厂默认值必须**原样穿过**钳制（否则默认配置自己就被改了，最坏的一种）
  const AlertsConfig def{};
  AlertsConfig same = def;
  alerts_config_clamp(same);
  TEST_ASSERT_EQUAL_FLOAT(def.overspeed_kmh, same.overspeed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(def.redline_rpm, same.redline_rpm);
  TEST_ASSERT_EQUAL_UINT32(def.turn_signal_on_ms, same.turn_signal_on_ms);
  TEST_ASSERT_EQUAL_UINT32(def.beep_ms, same.beep_ms);
  TEST_ASSERT_EQUAL_UINT32(def.beep_min_interval_ms, same.beep_min_interval_ms);

  // ① 超速：太大 ⇒ 永远不触发；负数 ⇒ 一开车就报。两边都要夹住
  c = AlertsConfig{};
  c.overspeed_kmh = 5000.0f;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_FLOAT(kAlertsOverspeedMaxKmh, c.overspeed_kmh);
  c = AlertsConfig{};
  c.overspeed_kmh = -10.0f;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_FLOAT(kAlertsOverspeedMinKmh, c.overspeed_kmh);

  // ② 红区：上限 12000（表盘到 7000、断油 6300 —— 写 99999 等于永不报）
  c = AlertsConfig{};
  c.redline_rpm = 99999.0f;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_FLOAT(kAlertsRedlineMaxRpm, c.redline_rpm);

  // ③ 转向忘关：★ 下限**不是 0** —— 0 就是"一打转向灯立刻报忘关"
  c = AlertsConfig{};
  c.turn_signal_on_ms = 0;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_UINT32(kAlertsTurnMinMs, c.turn_signal_on_ms);

  // ④ 单次响的时长：★ 0 就是"响了 0 毫秒" = 永远听不见
  c = AlertsConfig{};
  c.beep_ms = 0;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_UINT32(kAlertsBeepMinMs, c.beep_ms);
  c.beep_ms = 999999u;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_UINT32(kAlertsBeepMaxMs, c.beep_ms);

  // ⑤ 最短重复间隔：**0 是合法的**（= 能连着响，很吵但由人定），上限夹住即可
  c = AlertsConfig{};
  c.beep_min_interval_ms = 0;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_UINT32(0u, c.beep_min_interval_ms);
  c.beep_min_interval_ms = 999999u;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_UINT32(kAlertsBeepGapMaxMs, c.beep_min_interval_ms);

  // ⑥ NaN：不特判的话它会**穿过**两边的比较，之后所有阈值判定都变 false
  //    ⇒ 那条告警永远不触发。必须落回下限。
  c = AlertsConfig{};
  c.overspeed_kmh = 0.0f / 0.0f;   // NaN（不用 NAN 宏，免得依赖 <math.h>）
  c.redline_rpm = 0.0f / 0.0f;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_FLOAT(kAlertsOverspeedMinKmh, c.overspeed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(kAlertsRedlineMinRpm, c.redline_rpm);
  TEST_ASSERT_FALSE(c.overspeed_kmh != c.overspeed_kmh);   // 确实不是 NaN 了

  // ⑦ 合法范围内的值**一个都不许动**（钳制不能变成"顺手改成默认值"）
  c = AlertsConfig{};
  c.overspeed_kmh = 90.0f;
  c.redline_rpm = 6000.0f;
  c.turn_signal_on_ms = 12000u;
  c.beep_ms = 200u;
  c.beep_min_interval_ms = 1000u;
  c.only_highest = false;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_FLOAT(90.0f, c.overspeed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(6000.0f, c.redline_rpm);
  TEST_ASSERT_EQUAL_UINT32(12000u, c.turn_signal_on_ms);
  TEST_ASSERT_EQUAL_UINT32(200u, c.beep_ms);
  TEST_ASSERT_EQUAL_UINT32(1000u, c.beep_min_interval_ms);
  TEST_ASSERT_FALSE(c.only_highest);
}

// 钳制之后的那份配置**真的会被告警层用**（不是钳了个摆设）：
// 把超速阈值设到 90，车速 95 就该报 —— 而同一个 95 在默认 120 下不该报。
void test_alerts_clamped_config_is_actually_used(void) {
  VehicleState s = clean();
  s.speed_kmh = 95.0f;

  Alerts a;                       // 默认阈值 120
  uint32_t t = 1000;
  for (; t <= 4000; t += 100) TEST_ASSERT_EQUAL((int)AlertKind::None, (int)a.update(s, t));

  AlertsConfig c;
  c.overspeed_kmh = 90.0f;
  c.overspeed_hyst_kmh = 3.0f;
  alerts_config_clamp(c);
  Alerts b;
  b.setConfig(c);
  AlertKind got = AlertKind::None;
  for (t = 1000; t <= 4000; t += 100) got = b.update(s, t);
  TEST_ASSERT_EQUAL((int)AlertKind::Overspeed, (int)got);
}

void register_alerts_tests(void) {
  RUN_TEST(test_alerts_idle_by_default);
  RUN_TEST(test_alerts_overspeed_with_hysteresis);
  RUN_TEST(test_alerts_redline);
  RUN_TEST(test_alerts_beep_repeat_interval);
  RUN_TEST(test_alerts_mute_switch);
  RUN_TEST(test_alerts_door_uses_activity);
  RUN_TEST(test_alerts_turn_signal_left_on);
  RUN_TEST(test_alerts_hazard_is_not_forgotten_signal);
  RUN_TEST(test_alerts_priority_and_single_output);
  RUN_TEST(test_alerts_reset_semantics);
  RUN_TEST(test_buzzer_abstraction);
  RUN_TEST(test_alerts_config_clamp_ranges);
  RUN_TEST(test_alerts_clamped_config_is_actually_used);
}

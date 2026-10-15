// ============================================================
// 告警层用例（2026-09-24）
//
// 三条规则的每一条都要有**正反两面**（这才叫测掉了）:
//   ① 去抖:短于窗口不报 / 连续够了才报 / 中途断掉要**重新计时**
//   ② 迟滞:进用阈值、退用阈值减迟滞（贴着边界不许来回抖）
//   ③ 优先级:同时命中多条时只报最高的那一条
// 外加:只认已解字段(双闪不算"忘关")、reset 的语义、
//       以及**配置钳制**（那份配置现在能从 theme.json 来）。
//
// ★★ 2026-10-15：蜂鸣器整车砍掉（车主原话"蜂鸣器功能砍掉，节省资源"）⇒
//   原来那批 `beepCount()` / `beeping()` / `pattern()` / 静音开关的断言全部
//   改写或删除。**判据一条都没少**：本文件现在钉的就是"该报哪一条"，
//   而不再是"响了几声"。
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

// 造一个干净的 VehicleState（默认值里 speed=0 / rpm=0，不会误触发）
static VehicleState clean() {
  VehicleState s{};
  s.speed_kmh = 0.0f;
  s.rpm = 0.0f;
  return s;
}

// ============================================================
// 一、什么都不满足 ⇒ 一条都不报
// ============================================================
void test_alerts_idle_by_default(void) {
  Alerts a;
  const VehicleState s = clean();
  for (uint32_t t = 1000; t <= 5000; t += 100) {
    TEST_ASSERT_TRUE(a.update(s, t) == AlertKind::None);
  }
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);

  // ★ 只为"未解字段不给告警"这条纪律留个反证:水温/进气/油量乱填到危险值,
  //   也**一条都不许触发**（它们要么是 OBD 的、要么根本没解出来）。
  VehicleState hot{};
  hot.coolant_c = 130.0f;
  hot.intake_c = 90.0f;
  hot.fuel_pct = 1.0f;
  TEST_ASSERT_TRUE(a.update(hot, 5100) == AlertKind::None);
}

// ============================================================
// 二、超速:进 150 / 退 147(迟滞)
//
// ★★ 2026-10-15：阈值 **120 → 150**（车主拍板："速度表超过 150KM/H 的时候也红吧"）。
//   理由与红区那条一样：**不留两套不一致的阈值**（告警一个数、屏上另一个数，
//   车主就永远看不出提示）。下面的断言**一律走 `a.config()`**，所以改阈值不用改
//   这里的数字 —— 只有注释里的数要跟着走。
// ============================================================
void test_alerts_overspeed_with_hysteresis(void) {
  Alerts a;
  VehicleState s = clean();

  // 阈值下 0.1：连跑 3 秒都不报（顺带证明"没到阈值就是没到,与时间无关"）
  s.speed_kmh = a.config().overspeed_kmh - 0.1f;
  for (uint32_t t = 1000; t <= 4000; t += 100) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);

  // 到阈值:去抖窗口 250 ms **差一点**不够 ⇒ 还不报（4500 - 4400 = 100）
  s.speed_kmh = a.config().overspeed_kmh;
  a.update(s, 4400);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  // 够 250 ms ⇒ 报
  a.update(s, 4650);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Overspeed);

  // 掉到"阈值 - 迟滞 + 1"（在迟滞带里）⇒ **继续报**（不来回跳）
  s.speed_kmh = a.config().overspeed_kmh - a.config().overspeed_hyst_kmh + 1.0f;
  a.update(s, 4750);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Overspeed);

  // 掉到"阈值 - 迟滞 - 1"（出迟滞带）⇒ 立刻清（恢复不需要去抖）
  s.speed_kmh = a.config().overspeed_kmh - a.config().overspeed_hyst_kmh - 1.0f;
  a.update(s, 4850);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);

  // 回到迟滞带内（> 退出门槛但 < 进入门槛）⇒ 仍然不报 —— 迟滞是**两侧**的
  s.speed_kmh = a.config().overspeed_kmh - 1.0f;
  for (uint32_t t = 4950; t <= 7000; t += 100) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
}

// ============================================================
// 三、转速红区:进 5500 / 退 5300（迟滞 200）
//
// ★★ 2026-10-15：阈值 **5800 → 5500**（车主拍板："红区提示 5500"）。
//   与 `expression.cpp` 的 `kRpmRedlineFrom`、`face_stages.h` 的阶段表行
//   是**同一个触发点**（三处一起改，理由见 alerts.h 那一段）。
// ============================================================
void test_alerts_redline(void) {
  Alerts a;
  VehicleState s = clean();

  s.rpm = a.config().redline_rpm - 10.0f;        // 阈值下 10 转 ⇒ 不报
  for (uint32_t t = 1000; t <= 3000; t += 100) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);

  s.rpm = a.config().redline_rpm;                // = 阈值 ⇒ 报
  a.update(s, 3100);
  a.update(s, 3400);                             // 300 ≥ 250 的去抖窗口
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);

  // 退到"阈值 - 迟滞 + 100"（仍在迟滞带里）⇒ 继续报
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
// ============================================================
// 四、★ 2026-10-15 改写：**同一条告警一直成立 ⇒ 一直报同一条，不会自己消失**
//
//   这一条原来是"最短重复间隔"（`beepCount()` 每 3 秒 +1）的用例。蜂鸣器砍掉之后
//   那个读数没有了，但它**底下要防的那件事还在**：条件持续成立时，
//   `Alerts` 不许把它抖掉、不许换档、也不许"报一次就清"。
//   ⇒ 判据从"响了几声"换成"active() 15 秒里一个字节都不变"，钉的是同一个不变式。
// ============================================================
void test_alerts_redline_stays_latched(void) {
  Alerts a;
  VehicleState s = clean();
  s.rpm = 6000.0f;

  a.update(s, 1000);        // 条件首次成立 ⇒ 去抖窗口从这一刻起
  a.update(s, 1100);        // 100 ms:还在去抖
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  a.update(s, 1300);        // 300 ms ≥ 250 ⇒ 报
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);

  // 一路推到 16300（15 秒）：**每一点都必须是同一条**，一次都不许闪断
  for (uint32_t t = 1400; t <= 16300; t += 100) {
    a.update(s, t);
    TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
  }
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
}

// ============================================================
// 五、★ 2026-10-15 改写：**没到阈值就是没到**（去抖不能被"时间"糊过去）
//
//   这一条原来是"静音开关"的用例（掐蜂鸣器、不掐屏上那条）。
//   静音随蜂鸣器一起删了，改钉同一段的另一半：**一直没到阈值时，
//   不管跑多久都不许报** —— 也就是"去抖窗口只对'条件成立'计时"这件事。
// ============================================================
void test_alerts_below_threshold_never_reports(void) {
  Alerts a;
  VehicleState s = clean();
  s.rpm = a.config().redline_rpm - 1.0f;      // 差 1 转

  // 跑 15 秒，每一拍都不许报（"等久一点就会报"是最容易写歪的一种状态机）
  for (uint32_t t = 1000; t <= 16000; t += 100) {
    a.update(s, t);
    TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  }

  // 补上那 1 转 ⇒ 从这一刻起算去抖（不是"立刻报"）
  s.rpm = a.config().redline_rpm;
  a.update(s, 16100);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);      // 100 ms < 250
  a.update(s, 16400);                                   // 300 ms ≥ 250
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
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
}

// ============================================================
// 八、优先级:同时命中时只报最高那一条,且顺序稳定
// ============================================================
void test_alerts_priority_and_single_output(void) {
  Alerts a;
  VehicleState s = clean();
  const uint32_t kTurnOn = 1000;

  // 四条全命中:超速 160 + 红区 6000 + 门活动 + 左转亮着
  // ★★ 2026-10-15：车速从 130 提到 **160** —— 超速阈值当天从 120 涨到 150，
  //   而这一条测的是"四条同时成立时挑最重的那一条"，所以那一位**必须真的成立**
  //   （130 现在不构成超速，用例会当场红 —— 那条红是**对的**，它逼着这里跟着改）。
  s.speed_kmh = 160.0f;
  s.rpm = 6000.0f;
  s.door_activity = true;
  s.indicator_left = true;
  s.hazard = false;

  // 走到"转向那条也够 20 秒"之后（此时四条都成立）
  const uint32_t tEnd = kTurnOn + a.config().turn_signal_on_ms + 1000u;
  for (uint32_t t = kTurnOn; t <= tEnd; t += 250) a.update(s, t);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Overspeed);       // 最高

  // 车速掉下去 ⇒ 下一条顶上（红区）
  s.speed_kmh = 0.0f;
  a.update(s, tEnd + 250);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);

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
}

// ============================================================
// 九、reset() 的语义:清状态机（换数据源时不带上一路"还在报"过来）
//
// ★★ 2026-10-15：原来是"清状态机、**不清**静音与计数"。蜂鸣器砍掉之后
//   "静音与计数"都不存在了 ⇒ 这一条只剩前半条，但**要防的事一模一样**：
//   reset 之后条件仍成立时，必须**重新去抖**（不是"立刻又报"）。
// ============================================================
void test_alerts_reset_semantics(void) {
  Alerts a;
  VehicleState s = clean();
  s.rpm = 6000.0f;
  a.update(s, 1000);
  a.update(s, 1300);
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);

  a.reset();
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);

  // 复位之后条件仍成立 ⇒ 要**重新**去抖（不是"立刻又报"）
  a.update(s, 5000);
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);   // 计时窗口从这一刻重开
  a.update(s, 5200);                                 // 200 < 250 ⇒ 还不报
  TEST_ASSERT_TRUE(a.active() == AlertKind::None);
  a.update(s, 5300);                                 // 300 ≥ 250 ⇒ 报
  TEST_ASSERT_TRUE(a.active() == AlertKind::Redline);
}

// ============================================================
// 十、★ 2026-10-15：告警名（日志/诊断页用）逐条对账
//
//   这一组原来是"蜂鸣器抽象本身"（`BuzzerNull`/`BuzzerHost` 各钉一条 +
//   `patternFor()` 四条互不相同）。蜂鸣器与 `BeepPattern` 整车砍掉 ⇒
//   那部分断言删了；留下的这一半**照旧重要**：`alertName()` 是串口日志
//   （`alert: redline`）与诊断页那一格的取值来源，有一个空洞就会打出 "?"。
// ============================================================
void test_alert_names_have_no_holes(void) {
  TEST_ASSERT_EQUAL_STRING("none", alertName(AlertKind::None));
  TEST_ASSERT_EQUAL_STRING("overspeed", alertName(AlertKind::Overspeed));
  TEST_ASSERT_EQUAL_STRING("redline", alertName(AlertKind::Redline));
  TEST_ASSERT_EQUAL_STRING("door", alertName(AlertKind::Door));
  TEST_ASSERT_EQUAL_STRING("turn-signal", alertName(AlertKind::TurnSignal));

  // ★ 枚举里**每一条**都要有名字（含 Count 那个哨兵不要紧，但合法的几条必须有）
  for (uint8_t i = 0; i < (uint8_t)AlertKind::Count; ++i) {
    const char* n = alertName((AlertKind)i);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_TRUE(strlen(n) > 0);
    TEST_ASSERT_TRUE(strcmp(n, "?") != 0);
  }
  // 越界的值走兜底（不许崩、也不许返回空指针）
  TEST_ASSERT_EQUAL_STRING("?", alertName((AlertKind)200));
}

// ============================================================
// 十一、配置钳制（2026-09-27 新增）
//
// 为什么要有它：这份配置现在**能从主题文件来**（编辑器里拖控件改，见
// lib/themetool/theme_store.cpp 的 theme_parse_alerts_json）⇒ 它从"编译期常量"
// 变成了"外部输入"。下面每一条都对应一个"手滑写进去、屏上和日志上都看不出来"
// 的坑 —— 所以每条都验**边界值本身**，而不是"随便钳一下"。
//
// ★★ 2026-10-15：`beep_ms` / `beep_min_interval_ms` 两条**照旧验** ——
//   蜂鸣器虽然删了，但这两个键还住在 `theme.json` 的 `alerts` 段里、
//   解析器照旧读它们、`alerts_config_clamp()` 照旧钳它们（删了钳制会让
//   老主题文件"看起来坏了"）。它们现在只是**没有消费方**。
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
// 把超速阈值设到 90，车速 95 就该报 —— 而同一个 95 在默认阈值下不该报。
// ★★ 2026-10-15：默认阈值从 120 改成 150 之后，这仍然成立（95 < 150）；
//   而且这里**刻意走两条不同的路**（一条用默认、一条用 setConfig），
//   于是"阈值到底是谁说了算"这件事在这里被钉死。
void test_alerts_clamped_config_is_actually_used(void) {
  VehicleState s = clean();
  s.speed_kmh = 95.0f;

  Alerts a;                       // 默认阈值（现在是 150）
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

// ★★ 2026-10-15 新增：**默认阈值本身就是这次要的口径**
//   （车主："速度表超过 150KM/H 的时候也红吧" ⇒ `overspeed_kmh = 150`）。
//   ★ 为什么把默认值也钉一遍：它是固件层面"屏上那条红色提示什么时候出现"的
//     唯一出处（主题文件没写 `alerts.overspeed_kmh` 时就是它）。
//     与红区那条同理（见下一条）。
void test_alerts_default_thresholds_are_the_agreed_ones(void) {
  const AlertsConfig def{};
  TEST_ASSERT_EQUAL_FLOAT(150.0f, def.overspeed_kmh);   // 车主口径：150
  TEST_ASSERT_EQUAL_FLOAT(3.0f, def.overspeed_hyst_kmh);  // 迟滞保留
  TEST_ASSERT_EQUAL_FLOAT(5500.0f, def.redline_rpm);    // 车主口径：5500
  TEST_ASSERT_EQUAL_FLOAT(200.0f, def.redline_hyst_rpm);  // 迟滞保留
  // 钳制允许这两个值原样穿过（`kAlertsOverspeedMaxKmh` = 300、
  // `kAlertsRedlineMinRpm` = 0 ⇒ 150 与 5500 都在范围内）
  AlertsConfig c = def;
  alerts_config_clamp(c);
  TEST_ASSERT_EQUAL_FLOAT(150.0f, c.overspeed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(5500.0f, c.redline_rpm);
}

void register_alerts_tests(void) {
  RUN_TEST(test_alerts_idle_by_default);
  RUN_TEST(test_alerts_overspeed_with_hysteresis);
  RUN_TEST(test_alerts_redline);
  RUN_TEST(test_alerts_redline_stays_latched);
  RUN_TEST(test_alerts_below_threshold_never_reports);
  RUN_TEST(test_alerts_door_uses_activity);
  RUN_TEST(test_alerts_turn_signal_left_on);
  RUN_TEST(test_alerts_hazard_is_not_forgotten_signal);
  RUN_TEST(test_alerts_priority_and_single_output);
  RUN_TEST(test_alerts_reset_semantics);
  RUN_TEST(test_alert_names_have_no_holes);
  RUN_TEST(test_alerts_config_clamp_ranges);
  RUN_TEST(test_alerts_clamped_config_is_actually_used);
  RUN_TEST(test_alerts_default_thresholds_are_the_agreed_ones);
}

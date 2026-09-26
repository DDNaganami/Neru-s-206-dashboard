#include "alerts.h"

// 钳制助手（文件内）：把 v 夹进 [lo, hi]。
// ★ 先判 NaN：`v != v` 为真时任何比较都是 false ⇒ 不特判的话 NaN 会**穿过**
//   两边的夹子活下来，之后所有阈值比较都变 false ⇒ 那条告警**永远不触发**
//   （屏上、日志上都看不出来，是最难查的一种）。
static float clampF(float v, float lo, float hi) {
  if (v != v) return lo;                 // NaN
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}
static uint32_t clampU(uint32_t v, uint32_t lo, uint32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// 见 alerts.h 里那道说明：这份配置现在能从主题文件来 ⇒ 越界值必须在**一处**被挡。
void alerts_config_clamp(AlertsConfig& c) {
  c.overspeed_kmh       = clampF(c.overspeed_kmh, kAlertsOverspeedMinKmh, kAlertsOverspeedMaxKmh);
  c.overspeed_hyst_kmh  = clampF(c.overspeed_hyst_kmh, 0.0f, kAlertsHystMaxKmh);
  c.redline_rpm         = clampF(c.redline_rpm, kAlertsRedlineMinRpm, kAlertsRedlineMaxRpm);
  c.redline_hyst_rpm    = clampF(c.redline_hyst_rpm, 0.0f, kAlertsRedlineHystMax);
  c.door_debounce_ms    = clampU(c.door_debounce_ms, kAlertsDebounceMinMs, kAlertsDebounceMaxMs);
  c.turn_signal_on_ms   = clampU(c.turn_signal_on_ms, kAlertsTurnMinMs, kAlertsTurnMaxMs);
  c.debounce_ms         = clampU(c.debounce_ms, kAlertsDebounceMinMs, kAlertsDebounceMaxMs);
  c.beep_min_interval_ms = clampU(c.beep_min_interval_ms, kAlertsBeepGapMinMs, kAlertsBeepGapMaxMs);
  c.beep_ms             = clampU(c.beep_ms, kAlertsBeepMinMs, kAlertsBeepMaxMs);
  // only_highest 是 bool，没有"越界"这回事（JSON 那边 1/0 与 true/false 都收）
}

const char* alertName(AlertKind k) {
  switch (k) {
    case AlertKind::None:       return "none";
    case AlertKind::Overspeed:  return "overspeed";
    case AlertKind::Redline:    return "redline";
    case AlertKind::Door:       return "door";
    case AlertKind::TurnSignal: return "turn-signal";
    case AlertKind::Count:      break;
  }
  return "?";
}

const char* beepPatternName(BeepPattern p) {
  switch (p) {
    case BeepPattern::Silent: return "silent";
    case BeepPattern::Short:  return "short";
    case BeepPattern::Triple: return "triple";
    case BeepPattern::Urgent: return "urgent";
    case BeepPattern::Long:   return "long";
    case BeepPattern::Count:  break;
  }
  return "?";
}

BeepPattern Alerts::patternFor(AlertKind k) {
  switch (k) {
    // 超速最急（四短）；红区按 README「下一步 #6」那条"响一声"的口径给三短；
    // 门给一长（人上下车时听着最明确）；转向灯只给一短（提醒，别烦人）。
    case AlertKind::Overspeed:  return BeepPattern::Urgent;
    case AlertKind::Redline:    return BeepPattern::Triple;
    case AlertKind::Door:       return BeepPattern::Long;
    case AlertKind::TurnSignal: return BeepPattern::Short;
    default:                    return BeepPattern::Silent;
  }
}

// 去抖通用段：条件**连续**成立 need_ms 才算。
// ★ 写成 `Alerts` 的**私有成员函数**（而不是 .cpp 里的自由函数）：
//   `Cond` 是私有嵌套类型，自由函数拿不到它（编译期就报
//   "'Cond' is a private member of 'Alerts'"）。
// ★ 只推进计时窗口，判定留给调用方读 `c.on` —— 因为转向灯那条用的不是
//   debounce_ms 而是 turn_signal_on_ms（那个数同时承担"忘关多久算忘"的语义，
//   两者是同一个计时器，不该各记一个）。
void Alerts::debounce(Cond& c, bool raw, uint32_t now_ms, uint32_t need_ms) {
  if (!raw) {
    c.on = false;
    c.run = false;
    return;
  }
  if (!c.run) {
    c.run = true;
    c.since_ms = now_ms;
  }
  if (!c.on && (uint32_t)(now_ms - c.since_ms) >= need_ms) c.on = true;
}

AlertKind Alerts::update(const VehicleState& s, uint32_t now_ms) {
  // ---------- ① 超速（带迟滞）----------
  // 迟滞是为了"在阈值上下不要哒哒叫"：进用 overspeed_kmh，退用减掉迟滞的值。
  const bool prev_over = raw_[(uint8_t)AlertKind::Overspeed];
  raw_[(uint8_t)AlertKind::Overspeed] =
      s.speed_kmh >= (prev_over ? (cfg_.overspeed_kmh - cfg_.overspeed_hyst_kmh)
                                : cfg_.overspeed_kmh);

  // ---------- ② 转速红区（带迟滞）----------
  const bool prev_red = raw_[(uint8_t)AlertKind::Redline];
  raw_[(uint8_t)AlertKind::Redline] =
      s.rpm >= (prev_red ? (cfg_.redline_rpm - cfg_.redline_hyst_rpm)
                         : cfg_.redline_rpm);

  // ---------- ③ 门（用"动过"，不是"门开着"）----------
  // ★ 值来自 vehicle_state.door_activity，那一格的含义见 van_source.h：
  //   左右门不可分辨 = 未解（§6 撤回①），所以判据只能是"信号动过"。
  raw_[(uint8_t)AlertKind::Door] = s.door_activity;

  // ---------- ④ 转向灯亮太久 ----------
  // ★ 双闪**不算**"忘关"：双闪是人主动按下去的，而且本来就允许长期亮
  //   （临时停车）。踩双闪时 `indicator_left/right` 两位都是 1，所以上面
  //   那条会命中 ⇒ 这里显式排除。
  raw_[(uint8_t)AlertKind::TurnSignal] =
      (s.indicator_left || s.indicator_right) && !s.hazard;

  debounce(cond_[(uint8_t)AlertKind::Overspeed], raw_[(uint8_t)AlertKind::Overspeed],
           now_ms, cfg_.debounce_ms);
  debounce(cond_[(uint8_t)AlertKind::Redline], raw_[(uint8_t)AlertKind::Redline],
           now_ms, cfg_.debounce_ms);
  debounce(cond_[(uint8_t)AlertKind::Door], raw_[(uint8_t)AlertKind::Door],
           now_ms, cfg_.door_debounce_ms);
  // 转向灯：窗口用 turn_signal_on_ms
  debounce(cond_[(uint8_t)AlertKind::TurnSignal], raw_[(uint8_t)AlertKind::TurnSignal],
           now_ms, cfg_.turn_signal_on_ms);

  // ---------- 选一条 ----------
  // 优先级 = 枚举顺序（超速 > 红区 > 门 > 转向），与下标一致。
  // ★ 屏上只有一个蜂鸣器、一个提示灯位 ⇒ 同时报等于都听不清/看不清，
  //   所以默认只挑一条（`only_highest`）。z 顺序里第一条命中的就是最高优先级。
  AlertKind pick = AlertKind::None;
  for (uint8_t i = 1; i < (uint8_t)AlertKind::Count; ++i) {
    if (!cond_[i].on) continue;
    if (pick == AlertKind::None) pick = (AlertKind)i;
    if (cfg_.only_highest) break;
  }

  // ---------- 蜂鸣器 ----------
  // 换了一条告警 ⇒ 重新计时（可以立刻响）；同一条持续 ⇒ 按最短重复间隔续响。
  if (pick != active_) {
    active_ = pick;
    beeped_ = false;
    last_beep_ms_ = 0;
  }
  beeping_ = false;
  if (active_ != AlertKind::None && !muted_) {
    // ★ "别让它一直叫"的两条上界都在这里：
    //   ① 一声只响 beep_ms（标称值，由 Buzzer 实现决定怎么落）；
    //   ② 两次响之间至少隔 beep_min_interval_ms —— 条件一直成立也只是"每
    //      beep_min_interval_ms 响一声"，不会变成长鸣。
    const bool due = !beeped_ ||
                     ((uint32_t)(now_ms - last_beep_ms_) >= cfg_.beep_min_interval_ms);
    if (due) {
      beeping_ = true;
      beeped_ = true;
      last_beep_ms_ = (now_ms == 0u) ? 1u : now_ms;   // 0 保留给"还没响过"
      beep_count_++;
    }
  }
  pattern_ = beeping_ ? patternFor(active_) : BeepPattern::Silent;
  return active_;
}

void Alerts::reset() {
  for (uint8_t i = 0; i < (uint8_t)AlertKind::Count; ++i) {
    cond_[i].on = false;
    cond_[i].run = false;
    cond_[i].since_ms = 0;
    raw_[i] = false;
  }
  active_ = AlertKind::None;
  pattern_ = BeepPattern::Silent;
  beeping_ = false;
  beeped_ = false;
  last_beep_ms_ = 0;
  // ★ 刻意不清 muted_ 与 beep_count_：静音是人的选择（换数据源不该忘掉），
  //   计数是累计统计（清了就查不出"这一趟响了几次"）。
}

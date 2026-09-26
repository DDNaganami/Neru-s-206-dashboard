#pragma once
#include <stdint.h>
#include "vehicle_state.h"

// ============================================================
// 告警层 —— 把"已经解出来的字段"变成"该不该响、响什么"
//
// ★★ 触发源**只用已实测解出的字段**（这一条是硬纪律，别扩）：
//     ① 超速     —— 车速档位（`0x824.data[2]` × 2.56，2026-09-22 实测定标）
//     ② 转速红区 —— 5800（表盘刻度到 7000、断油 6300，取 5800 提前报）
//     ③ 门未关   —— `0x4FC.data[1]` 的**活动**（⚠ 不是"门开着"：左右门不可
//                    分辨 = 未解，见 VAN-PROTOCOL §6 撤回①；`==1 就是开着`
//                    被 §4.6 否掉，所以这里用的是"动过"）
//     ④ 转向灯忘关 —— 左/右转向灯连续亮着超过 N 秒（打灯忘了回）
//
//   ✗ **不为水温/进气做告警**：那两项是 OBD 的（`0105` / `010F`），
//     等有线 ELM327 接到板上、量出 `SRC-Hz` 之后再说 —— 现在给它们做告警
//     等于为没有真值保证的数据立规矩。
//   ✗ **不为未解字段建空壳告警**（油量 / 挡位 / 里程 / 刹车 / 雨刮）：一个都不加。
//
// ---- 这一层为什么独立成一个文件（而不是写进 dash_ui 或 main）----
//   它**不碰 LVGL、不碰硬件、不依赖 Arduino**（纯逻辑 + 时间戳），于是：
//     · native 上能把去抖 / 限速 / 静音三条规则逐条测掉（test_alerts.cpp）；
//     · 模拟页（pcpreview）与真机跑**同一份判据**，"预览里响、车上不响"这种
//       分叉在结构上不可能发生；
//     · 蜂鸣器换成哪块板发（§8 的 L14）只换 Buzzer 的实现，本文件一个字不用动。
//
// ---- 三条必须有的规则（都是"别让它一直叫"）----
//   ① 去抖（debounce）：条件要**连续**成立 `debounce_ms` 才认。
//      为什么是"连续"而不是"累计"：门/灯在总线上是脉冲式的（§4.6 的段内跳变、
//      §4.3 的闪烁欠采样），累计计时会让一串噪声把告警"攒"出来。
//      恢复是**立即**的（条件一不成立就清），因为"不再报警"晚一步没有好处。
//   ② 最短重复间隔（beep_min_interval_ms）：响了之后至少隔这么久才允许再响。
//      ★ 它同时是"最长响多久"的上界——两个上限取小（见 update() 的说明）。
//   ③ 静音开关（muted）：led（上屏）与蜂鸣器**分开**处理 ——
//      静音只掐蜂鸣器，屏上的指示灯照旧（"静音"不等于"假装没事"）。
// ============================================================

enum class AlertKind : uint8_t {
  None = 0,
  Overspeed,     // 车速超阈值
  Redline,       // 转速进红区
  Door,          // 门信号有活动（不能叫"门开着"，见上）
  TurnSignal,    // 转向灯亮太久忘了回
  Count
};
const char* alertName(AlertKind k);

// ---- 蜂鸣器输出模式 ----
// ★ 是**模式**不是频率：具体频率/占空比由 Buzzer 那一侧决定（无源蜂鸣器要 PWM、
//   有源的只要高低电平），所以判据与驱动这样分层。
enum class BeepPattern : uint8_t {
  Silent = 0,    // 不响
  Short,         // 一短（转向灯忘关：提醒一下，别烦人）
  Triple,        // 三短（转速红区：与 README「下一步 #6」那条一致）
  Urgent,        // 四短（超速：最高优先级）
  Long,          // 一长（门未关：持续提示，人下车/上车时听着最明确）
  Count
};
const char* beepPatternName(BeepPattern p);

struct AlertsConfig {
  // ① 超速。★ 与表情那个超速档（>130，退 127）**刻意不同**：
  //    表情是"脸变成什么样"，告警是"要不要叫"。用 120/117 是因为
  //    车主手册那条路的限速不高于 120 —— 而且这条数是**阈值、不是定标**，
  //    改它不动任何协议结论。真车标定后只改这一个数。
  float overspeed_kmh = 120.0f;
  float overspeed_hyst_kmh = 3.0f;   // 退出迟滞（防止在阈值上下"哒哒"叫）

  // ② 转速红区。5800 与 expression.cpp 的 kRpmRedlineFrom 同一个地标
  //    （表盘 7000、断油 6300，提前到 5800 报）。这里**复述**一个数而不是
  //    去 include expression 的内部常量：那边是"表情档位"的口径，
  //    这边是"告警阈值"的口径，将来可能各自调。test_alerts.cpp 有一条用例
  //    钉住"两者现在相等"，调歪了会红。
  float redline_rpm = 5800.0f;
  float redline_hyst_rpm = 200.0f;

  // ③ 门。用 `door_activity`（"动过"），连续成立这么久才报。
  uint32_t door_debounce_ms = 200;

  // ④ 转向灯忘关：连续亮着这么久就报。20 秒是**工程判断**（一个完整路口
  //    转弯 + 变道在 10 秒量级，20 秒足够区分"正在转弯"与"忘了回"），
  //    不是实测值 —— 真车标定要 owner 自己试。
  uint32_t turn_signal_on_ms = 20000;

  // 通用：任意告警的去抖下限（0 = 用各自的）
  uint32_t debounce_ms = 250;

  // 最短重复间隔：响过之后至少隔这么久才允许再响（三个模式共用这一个数）。
  // ★ 3000 ms 的来历：README「下一步 #6」那条是"进红区响一声"（不是长鸣），
  //   而人要能听清又不被吵，重复周期取秒级。改成 0 会变成"一直叫"。
  uint32_t beep_min_interval_ms = 3000;

  // 单次蜂鸣的"标称时长"（ms）。★ 它**不是**驱动侧的精确时长：
  //   Buzzer 的实现可以按自己的硬件给（PC 上是提示音、真机上是 PWM 通断），
  //   这里只用它做"最长响多久"的上界（见 update()）。
  uint32_t beep_ms = 120;

  // 只报最高优先级的那一个（同时命中多条时）。优先级：超速 > 红区 > 门 > 转向。
  // 为什么不是"同时响几条"：屏上只有一个蜂鸣器、一个灯位，同时报等于都听不清。
  bool only_highest = true;
};

// ------------------------------------------------------------
// ★★ 2026-09-27：这份配置现在**可以从主题文件来**（`theme.json` 的 `alerts` 段，
//    在编辑器里拖控件改）⇒ 它就成了"外部生成、人会手改"的输入，
//    和主题配色同一个性质。于是必须有一处**集中**回答"什么算越界"。
//
//    为什么非钳不可（三个例子，都是手滑就写出来的）：
//      · `turn_signal_on_ms = 0` ⇒ **一打转向灯就报"忘关"**（下限取 1000 ms，
//        因为一次转弯本来就是秒级的事）；
//      · `beep_ms = 0` ⇒ 蜂鸣器"响"0 毫秒 = 永远听不见（这是最难查的一种：
//        屏上照报、日志照打，就是不响）；
//      · `overspeed_kmh = 5000` ⇒ 超速告警永远不会触发，而屏上什么都看不出来。
//
//    ★ 它**只**被解析器（`theme_parse_alerts_json`）调用；`setConfig()` 照旧信任
//      调用方 —— 宿主机用例与预览注入要能故意设成怪值去验边界。
//    ★ 返回钳制后的对象（引用入参，就地把越界值改回来）。
void alerts_config_clamp(AlertsConfig& c);

// 各字段的安全范围（**唯一出处**：钳制用它，编辑器/用例要报"允许范围"也从这里读；
// 用例逐条钉住，见 test_alerts.cpp 的 `alerts_config_clamp` 那一组）。
static const float    kAlertsOverspeedMinKmh  = 0.0f;
static const float    kAlertsOverspeedMaxKmh  = 300.0f;
static const float    kAlertsHystMaxKmh       = 50.0f;
static const float    kAlertsRedlineMinRpm    = 0.0f;
static const float    kAlertsRedlineMaxRpm    = 12000.0f;
static const float    kAlertsRedlineHystMax   = 1000.0f;
static const uint32_t kAlertsDebounceMinMs    = 0u;
static const uint32_t kAlertsDebounceMaxMs    = 10000u;
static const uint32_t kAlertsTurnMinMs        = 1000u;    // ★ 不是 0：见上面"一打灯就报"
static const uint32_t kAlertsTurnMaxMs        = 600000u;  // 10 分钟
static const uint32_t kAlertsBeepMinMs        = 1u;       // ★ 不是 0：0 = 听不见
static const uint32_t kAlertsBeepMaxMs        = 5000u;
static const uint32_t kAlertsBeepGapMinMs     = 0u;       // 0 合法（= 能连着响，很吵但由人定）
static const uint32_t kAlertsBeepGapMaxMs     = 60000u;

class Alerts {
public:
  Alerts() = default;
  explicit Alerts(const AlertsConfig& cfg) : cfg_(cfg) {}

  // 推进状态机。now_ms 与 data_service 的 now 同一个时基。
  // 返回**当前该处理的那一条**告警（多条命中且 only_highest 时取优先级最高的）。
  AlertKind update(const VehicleState& s, uint32_t now_ms);

  // 清空状态机的记忆。用途与 face_reset() 一样：换数据源（模拟↔真 VAN）时
  // 别让上一路的"还在告警中"延续到新源上。★ **不清静音开关**（那是人的选择，
  // 换源不该把手动静音忘掉）。
  void reset();

  // ---- 静音开关 ----
  bool muted() const { return muted_; }
  void setMuted(bool m) { muted_ = m; }
  void toggleMuted() { muted_ = !muted_; }

  // ---- 只读状态（UI / 日志用）----
  AlertKind active() const { return active_; }         // 当前生效的那条（静音时也照报）
  bool isActive(AlertKind k) const {                    // 某一条现在生效吗
    return k != AlertKind::None && k == active_;
  }
  bool beeping() const { return beeping_; }             // 蜂鸣器这一拍该不该响
  BeepPattern pattern() const { return pattern_; }       // 当前模式（Silent = 不响）
  uint32_t beepCount() const { return beep_count_; }     // 累计响了几次（日志/用例用）
  uint32_t lastBeepMs() const { return last_beep_ms_; }
  const AlertsConfig& config() const { return cfg_; }
  void setConfig(const AlertsConfig& c) { cfg_ = c; }

  static BeepPattern patternFor(AlertKind k);

private:
  // 去抖状态。**必须放在头文件里**（不能藏在 .cpp 的匿名空间）：
  //   native 用例直接构造 Alerts 并逐拍 update()，状态得是对象的一部分、
  //   而且 reset() 要能一次清干净（见 .cpp）。
  struct Cond {
    bool on = false;          // 去抖后的"成立"
    bool run = false;         // 计时窗口开着吗（raw 成立期间为 true）
    uint32_t since_ms = 0;    // 窗口起点
  };
  // 去抖通用段（定义在 .cpp）。私有成员函数而不是自由函数：`Cond` 是私有类型。
  static void debounce(Cond& c, bool raw, uint32_t now_ms, uint32_t need_ms);
  Cond cond_[(uint8_t)AlertKind::Count] = {};
  bool raw_[(uint8_t)AlertKind::Count] = {};   // 上一拍的原始条件（迟滞用）
  uint32_t beep_count_ = 0;

  AlertsConfig cfg_{};
  AlertKind active_ = AlertKind::None;
  BeepPattern pattern_ = BeepPattern::Silent;
  bool beeping_ = false;
  bool muted_ = false;
  uint32_t last_beep_ms_ = 0;
  bool beeped_ = false;        // 这一轮告警里响过没有（"最短重复间隔"的另一半）
};

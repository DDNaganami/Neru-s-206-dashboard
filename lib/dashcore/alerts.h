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
//     · native 上能把去抖 / 迟滞 / 优先级三条规则逐条测掉（test_alerts.cpp）；
//     · 模拟页（pcpreview）与真机跑**同一份判据**，"预览里报、车上不报"这种
//       分叉在结构上不可能发生；
//     · 告警以后换**形态**（文字条 / 表盘变色 / 新的角标 / 任何发声装置）只换
//       视图或驱动那一侧，本文件一个字不用动。
//
// ★★ 2026-10-15（车主）：「**蜂鸣器功能砍掉，节省资源**」。
//   ⇒ 本层**只回答"现在该报哪一条"**，不再有任何"响/不响"的概念：
//     `BeepPattern` / `beepCount()` / `beeping()` / `pattern()` / `muted()` /
//     `beep_min_interval_ms` / `beep_ms` 全部随 `buzzer.*` 一起删掉了
//     （`lib/dashcore/buzzer.h` / `buzzer_exio.*` 两个实现文件也已删除）。
//   ★ **判断逻辑一个字都没动**：四条判据、去抖窗口、迟滞带、优先级、
//     `only_highest` 全部原样保留 —— 车主砍的是"发声"，不是"判断"。
//   ★ `AlertsConfig` 里 `beep_*` 两个字段**保留**：它们是 `theme.json` 的
//     `alerts` 段里既有的键（`theme_parse_alerts_json` 照旧解析、照旧钳制），
//     删字段会让"主题文件看起来坏了"。它们现在只是**没有消费方**。
//
// ---- 必须有的规则 ----
//   ① 去抖（debounce）：条件要**连续**成立 `debounce_ms` 才认。
//      为什么是"连续"而不是"累计"：门/灯在总线上是脉冲式的（§4.6 的段内跳变、
//      §4.3 的闪烁欠采样），累计计时会让一串噪声把告警"攒"出来。
//      恢复是**立即**的（条件一不成立就清），因为"不再报警"晚一步没有好处。
//   ② 只报最高优先级的那一条（`only_highest`）：同时命中多条时取最重的。
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

struct AlertsConfig {
  // ① 超速。★ 与表情那个超速档（>130，退 127）**刻意不同**：
  //    表情是"脸变成什么样"，告警是"要不要叫"。用 150 是因为**车主
  //    2026-10-15 拍板**："速度表超过 150KM/H 的时候也红吧"—— 而这条阈值
  //    同时驱动屏上那两条红色提示（弧染红 + 大数字变红），所以不再是"随便一个
  //    阈值、改它不动协议结论"：它就是要**在 150 那一刻看得见**。
  //    ★ 它仍然是**阈值、不是定标**：改它不动任何协议结论。
  //    ★★ 注意：这个数与 `overspeed_hyst_kmh` 一起，被**三个**地方读：
  //      本层的告警判据、`src/dash_ui.cpp` 的弧染色、以及大数字变红。
  //      ⇒ 谁也不许再写一份字面量（"两套阈值"正是车主这次要消掉的东西）。
  //    ★ 历史值 120（2026-09-24 ~ 2026-10-15）已按车主口径提高；下面**不再
  //      写"旧值 = 120.0f"这种字样** —— `tools/theme-editor/test-theme-json.js`
  //      会用正则从本结构体里抓默认值，注释里出现同样的赋值会把那条对账带歪。
  float overspeed_kmh = 150.0f;
  float overspeed_hyst_kmh = 3.0f;   // 退出迟滞（防止在阈值上下"哒哒"叫）

  // ② 转速红区。★★ 2026-10-15：**5800 → 5500**（车主拍板）。
  //    起因：屏上"看得见的红区"当时落在 **6200** 那一档，而告警在 5800
  //    ⇒ 5800~6200 之间屏上几乎没变化，车主一直以为"没有红区提示"。
  //   现在三处**同一个触发点 5500**：本处（告警）、`expression.cpp` 的
  //    `kRpmRedlineFrom`（脸变红）、`face_stages.h` 的阶段表行（编辑器预览）。
  //    为什么是 5500：断油 6300（车主实测 2026-09-18），5500 留 800 转提前量
  //    —— 2 档 100km/h 就在 5500 附近，是**真会开到**的那一格（原来是 5800）。
  //    ★ 退出迟滞 200 转**保留**（进 5500 / 退 5300），防止在阈值上下"哒哒"报。
  float redline_rpm = 5500.0f;
  float redline_hyst_rpm = 200.0f;

  // ③ 门。用 `door_activity`（"动过"），连续成立这么久才报。
  uint32_t door_debounce_ms = 200;

  // ④ 转向灯忘关：连续亮着这么久就报。20 秒是**工程判断**（一个完整路口
  //    转弯 + 变道在 10 秒量级，20 秒足够区分"正在转弯"与"忘了回"），
  //    不是实测值 —— 真车标定要 owner 自己试。
  uint32_t turn_signal_on_ms = 20000;

  // 通用：任意告警的去抖下限（0 = 用各自的）
  uint32_t debounce_ms = 250;

  // ★★ 2026-10-15：下面这两个**只剩"主题文件里认得的键"这一个身份**
  //   （车主砍掉蜂鸣器 ⇒ 本层不再算"响多久/隔多久"）。
  //   ★ 为什么**不一起删**：它们住在 `theme.json` 的 `alerts` 段里，
  //     编辑器与已经导出的主题文件都带着它们；删字段等于让那些文件
  //     "看起来坏了"（见 alerts.h 文件头那条说明）。
  //     解析与钳制照旧（`theme_parse_alerts_json` / `alerts_config_clamp`）。
  uint32_t beep_min_interval_ms = 3000;
  uint32_t beep_ms = 120;

  // 只报最高优先级的那一个（同时命中多条时）。优先级：超速 > 红区 > 门 > 转向。
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
//      · `beep_ms = 0` ⇒ （历史项）蜂鸣器"响"0 毫秒 = 永远听不见 —— 蜂鸣器已删，
//        但主题文件里这个键照旧解析、照旧钳制，免得老文件"看起来坏了"；
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
  // 别让上一路的"还在告警中"延续到新源上。
  void reset();

  // ---- 只读状态（UI / 日志用）----
  AlertKind active() const { return active_; }         // 当前生效的那一条
  bool isActive(AlertKind k) const {                    // 某一条现在生效吗
    return k != AlertKind::None && k == active_;
  }
  const AlertsConfig& config() const { return cfg_; }
  void setConfig(const AlertsConfig& c) { cfg_ = c; }

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

  AlertsConfig cfg_{};
  AlertKind active_ = AlertKind::None;
};

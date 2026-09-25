#include "buzzer_exio.h"

// ============================================================
// 实现说明（与 buzzer_exio.h 的文件头配套读；这里只写"怎么落"）
//
// ★ 这个文件**一个 I2C/Arduino 符号都不引用**：
//   输出走注入的回调，时钟走注入的函数 ⇒ native 用例能用假时钟逐格推进，
//   而设备侧把回调接在显示驱动的 `dash_buzzer_set()` 上（唯一的写者）。
// ============================================================

namespace {

// 把标称时长夹进 [1, kMaxPulseMs]。
// ★ 下限取 1 而不是 0："开 0ms"在这种"开 → 到点关"的模型里等于**没有关的时刻**
//   （`elapsed >= 0` 恒真，行为取决于电平原值），留 1ms 让它永远有明确的收尾。
//   `beep_ms == 0` 因此变成"极短的一声"，不是"长鸣"—— 这是**刻意的**默认方向。
uint32_t clampPulse(uint32_t ms) {
  if (ms == 0u) return 1u;
  return (ms > kMaxPulseMs) ? kMaxPulseMs : ms;
}

// 往落法里追加一相。★ `on_ms` 一律先夹再存 —— 序列里**不可能**出现
// 超过 kMaxPulseMs 的相（用例直接遍历这个数组来钉这条硬约束）。
void push(BeepPulses& ps, uint32_t on_ms) {
  if (ps.n >= (uint8_t)(sizeof(ps.on_ms) / sizeof(ps.on_ms[0]))) return;
  ps.on_ms[ps.n++] = clampPulse(on_ms);
}

}  // namespace

BeepPulses pulsesFor(BeepPattern pattern, uint32_t beep_ms) {
  BeepPulses ps{};
  ps.n = 0;
  const uint32_t on = clampPulse(beep_ms);

  switch (pattern) {
    case BeepPattern::Silent:
      // 静音：`beep()` 调用点本来就不该被调到（判据在 alerts/system_status），
      // 但真被调到时**一声都不许响** —— 不响比"响错一声"安全。
      break;

    case BeepPattern::Short:
      push(ps, on);                      // 1 声
      break;

    case BeepPattern::Triple:
      push(ps, on); push(ps, on); push(ps, on);   // 3 声
      break;

    case BeepPattern::Urgent:
      // 4 声（比 Triple 多一声 —— 这是"最急"那一档在**次数**上的落法）。
      // ★ 有源蜂鸣器没有频率，所以"更急"只能靠"更多声"来表达。
      push(ps, on); push(ps, on); push(ps, on); push(ps, on);
      break;

    case BeepPattern::Long:
      // ★★ **降级：长鸣 ⇒ 3 短哔**（见 buzzer_exio.h 文件头 ②）。
      //   这不是"实现偷懒"，是硬约束：那次黑屏事故的候选机制 B
      //   （持续高电平把 3.3V 轨拉低 ⇒ 面板掉状态）**还没被排除**，
      //   而 `ARCHITECTURE.md` §4 第 3 条写明了在排除前真机不许长鸣。
      //   ★ `alerts` 侧一个字没改：门告警照旧映射到 `BeepPattern::Long`，
      //     "怎么落"是驱动这一层的自由（这正是 buzzer.h 当初分层的理由）。
      push(ps, on); push(ps, on); push(ps, on);
      break;
  }
  return ps;
}

BuzzerExio::BuzzerExio(BuzzerExioSetFn set_fn, void* ctx, uint32_t (*now_fn)())
    : set_(set_fn), ctx_(ctx), now_(now_fn) {}

// ★ 只在**电平真的变了**的时候才动总线：`begin()` 与主循环每轮的 `off()`
//   都会走到这里，无条件写一遍会让"同一个电平反复写扩展器"变成常态
//   （真机上是白花 I2C 事务与日志，宿主机上则让用例数不清"到底动了几次"）。
//   ★ 这个缓存是**保守的**：它只记"我们要求过什么"，不对硬件做任何假设
//   （真正的对账是 `dash_buzzer_set()` 的回读自证那一行）。
void BuzzerExio::allOff() {
  if (out_on_) {
    out_on_ = false;
    if (set_ != nullptr) set_(false, ctx_);
  }
}

void BuzzerExio::startSequence(const BeepPulses& ps) {
  pulses_ = ps;
  pulse_idx_ = 0;
  active_ = (ps.n != 0u);
  pulse_start_ms_ = now_();
  // ★★ 绝对上限的**计时起点** = 整条序列起表的那一刻（见 `safety()`）。
  //   ★ 与 `pulse_start_ms_` 的分工：后者是"**当前相**什么时候开始的"（每一相都重置），
  //     前者是"**整条序列**什么时候开始的"（一次 beep() 只记一次）—— 上限要的是后者。
  seq_start_ms_ = pulse_start_ms_;
  last_pulse_ms_ = 0u;
  // 起手先给一个**已知**的电平：序列总是从"静音段"开始（第一相由 tick() 起），
  // 于是"从上一拍遗留的高电平接着算"这种事不可能发生。
  allOff();
}

void BuzzerExio::clearSequence() {
  // ★ 必须把 `cur_pattern_`/`cur_req_ms_` 一起清掉：`beep()` 靠
  //   "(active_ && 同模式同时长) ⇒ 什么都不做"来做幂等，而序列走完之后
  //   `active_` 已经是 false —— 留着那对缓存会让**下一次同模式同长的 beep
  //   被永久吞掉**（第 2 声再也响不了；那种 bug 的表现是"只有第一次会响"）。
  active_ = false;
  pulse_idx_ = 0;
  pulses_.n = 0;
  last_pulse_ms_ = 0u;
  cur_pattern_ = BeepPattern::Silent;
  cur_req_ms_ = 0u;
}

void BuzzerExio::begin() {
  // 上电静音。★ 不记忆任何硬件状态、也不去读：读-改-写那一份在显示驱动里，
  // 这里只发一个"关"的请求（对那颗芯片来说就是"把 EXIO8 写成 0"）。
  clearSequence();
  allOff();
}

void BuzzerExio::beep(BeepPattern pattern, uint32_t beep_ms) {
  // ★ 主循环**每轮**都调 `beep()`（见 main.cpp 的告警那一段）⇒ 必须幂等：
  //   同模式同时长、而且这一拍还在推进 ⇒ 什么都不做（别把相位归零）。
  if (active_ && pattern == cur_pattern_ && beep_ms == cur_req_ms_) return;

  cur_pattern_ = pattern;
  cur_req_ms_ = beep_ms;
  startSequence(pulsesFor(pattern, beep_ms));
}

void BuzzerExio::off() {
  // 「取消」：丢掉序列 + 立刻静默。★ 主循环在"不该响"的那些轮里就调它，
  // 所以这一条同时是"静音那一跳的掐断路径"。
  clearSequence();
  allOff();
}

bool BuzzerExio::pulseActive() const {
  // ★ 相号是**奇数**才是"响"：序列的排法固定是
  //   相 0 = 静音段（占位）、相 1 = 第一声、相 2 = 间隙、相 3 = 第二声 …
  //   ⇒ 起表那一刻一定是静音，第一声由 tick() 起（见 startSequence 的说明）。
  if (!active_) return false;
  return (pulse_idx_ % 2u) == 1u;
}

void BuzzerExio::tick() {
  if (!active_) return;
  const uint32_t now = now_();

  // ★★ 起表之后的**第一次 tick 只做一件事：起第一声**，不参与"相走完了没有"。
  //   为什么必须这么写（真机上踩得到）：主循环两轮之间可能有很长的间隔
  //   （RGB 那条路上一次整屏刷新的 flush 就是几十毫秒，开机那一下还要 ~1 秒），
  //   而"起表 → 第一次 tick"之间正好会跨过这种间隔。若把相 0（那个 0 长的
  //   占位静音相）交给下面的计时逻辑去走，那一次 tick 会因为
  //   `elapsed >= 0` 当场把它走完，于是**相位计数白推一格**：
  //   若这一推越过了整条序列（单相序列就是这种情况），这一声就**永远不响了**
  //   （`beep()` 记下了"在推进"，却一声都没发出）。
  //   ⇒ 把起表后的第一声做成**无条件**的，与间隔多大无关。
  if (pulse_idx_ == 0u) {
    if (pulses_.n == 0u) { clearSequence(); return; }   // 空序列：不该走到这儿
    const uint32_t on_ms = pulses_.on_ms[0];
    last_pulse_ms_ = on_ms;
    if (!out_on_) {
      out_on_ = true;
      if (set_ != nullptr) set_(true, ctx_);
    }
    pulse_idx_ = 1u;              // 进入"响"的那一相（相号奇数 = 响）
    pulse_start_ms_ = now;        // ★ 这一相的时长从**真的开**那一刻算
    return;
  }

  // 当前相是"响"还是"静音"：由**相号的奇偶**决定（见 pulseActive 的说明），
  // 于是槽位数组只管"每一相多长"。
  const bool is_on = ((pulse_idx_ % 2u) == 1u);
  const uint32_t dur = is_on
      ? pulses_.on_ms[pulse_idx_ / 2u]     // 响：第 (i-1)/2 声的时长
      : ((pulses_.n > 1u) ? kGapMs : 0u);  // 静音：只有多相序列才需要间隙

  // ★ 时钟万一倒退/跳变（注入的时钟在用例里是单调的，但真机上的 `millis()`
  //   在长阻塞之后会一次跳很远）⇒ 把起点**向前夹住**：宁可把这一相拉长到
  //   它的标称时长，也不让一次跳变把"响"的那一相**提前掐掉**再重新起一遍。
  if ((int32_t)(now - pulse_start_ms_) < 0) pulse_start_ms_ = now;
  const uint32_t elapsed = (uint32_t)(now - pulse_start_ms_);
  if (elapsed < dur) return;   // 还没到点

  if (is_on) {
    allOff();                  // 到点就关（这是"≤300ms"那条硬约束的落点）
    ++pulse_idx_;              // 进入间隙相
    // 最后一相（间隙）走完 ⇒ 这一拍结束。
    if (pulse_idx_ / 2u >= pulses_.n) {
      clearSequence();         // ★ 连幂等缓存一起清（否则下一次同模式的 beep 会被吞）
    } else {
      pulse_start_ms_ = now;
    }
    return;
  }

  // 静音相走完 ⇒ 起下一声。
  if ((pulse_idx_ / 2u) < pulses_.n) {
    const uint32_t on_ms = pulses_.on_ms[pulse_idx_ / 2u];
    last_pulse_ms_ = on_ms;
    if (!out_on_) {
      out_on_ = true;
      if (set_ != nullptr) set_(true, ctx_);
    }
    ++pulse_idx_;              // 进入"响"的那一相
    pulse_start_ms_ = now;
    return;
  }

  // 兜底：序列排空了却还在 active（不该发生）⇒ 收干净，别留着高电平。
  clearSequence();
  pulse_start_ms_ = now;
  allOff();
}

// ============================================================
// ★★ 绝对上限兜底（`Buzzer::safety()` 的真正实现）—— 2026-09-25 新增
//
// 起因（车主原话，逐字）："**现在会长鸣一会儿，画面也卡住了**"。
// 这块板上的蜂鸣器是**软开关**（写 TCA9554 的 EXIO8，没有硬件定时）⇒
// "到点关"那个动作**只在主循环转得动的时候**才被执行；主循环一停，
// `tick()` 就再也不会被调用，高电平就留在了总线上 —— **谁也关不掉**。
//
// ★ 所以这一条**刻意不写在 `tick()` 里面**：写在里面就只能防"序列走歪"，
//   防不住"tick() 压根没被调用" —— 而后者正是这次遇到的病。
//   调用方（`main.cpp` 主循环）每轮**独立**调它一次（与 `tick()` 并列）。
//
// ★ 判据只有一条：`now - seq_start_ms_ >= kBuzzerSafetyMs`（2 s，见头文件那段推导：
//   最长合法序列 1440 ms < 2000 ms ⇒ 合法序列永远不会被它掐）。
//   ★ 用**无符号差值**比较（不是"绝对值比大小"）：`millis()` 在 49.7 天回绕时，
//     差值形式天然正确；而且它在"时钟倒退"时给出的是一个巨大的数（>= 上限）⇒
//     同样是"关掉"这个安全方向，不会出现"永远不关"。
//   ★ `active_` 那道早退是**性能**上的：没有序列时这一条就是一个 `if`。
//     （不用它做正确性判断：真机上"留了高电平而 active_ 为假"这件事不该发生 ——
//       `allOff()` 在每次 clearSequence/off 里都会把输出拉低。）
//
// ★ 它**不动**既有那条"单次哔 ≤ 300 ms"的硬约束：序列自己照旧按相走、每相照旧被夹；
//   这一条只在"那段逻辑没机会跑"的时候生效 —— 是**天花板**，不是替代。
// ============================================================
void BuzzerExio::safety() {
  if (!active_) return;
  const uint32_t now = now_();
  if ((uint32_t)(now - seq_start_ms_) < kBuzzerSafetyMs) return;

  // 到这儿 = 一次 `beep()` 之后已经响了（或者本该响着）超过 2 秒 ⇒ 无条件收干净。
  ++safety_cuts_;
  clearSequence();   // 丢掉序列 + 幂等缓存（否则"同模式同长的下一次 beep"会被吞）
  allOff();          // 把输出拉回静默（`out_on_` 缓存一并清）
}

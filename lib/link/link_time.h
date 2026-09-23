#pragma once
#include <stdint.h>

#include "link_msg.h"

// ============================================================
// 双板链路协议 v1 —— **时基**（§4「时基与超时」+ §3 的 TICK / DATA 两行）
//
// 三件事，各归各的类/函数：
//   ① **主板侧** TickGen  —— 50 Hz 的 tick 节奏 + `seq`（`tick_ms` = 主板自己的
//      单调 `millis()`，从复位起算，32 位 ⇒ 49.7 天回绕，§4）；
//   ② **从板侧** LinkTime —— 收到 TICK 时算 `d = (int32_t)(tick_ms - local_ms)`
//      （有符号减法天然处理回绕），对 d 做**滑动最小 + 一阶低通**；三级超时
//      状态机（100 ms 失基准 / 500 ms 降级 / 3 s 回退 Sim）；
//   ③ **跨屏扫表对齐** msUntilGrid() —— 等到下一个 tick 栅格再启动自己的
//      扫表动画（§4 建议栅格 `tick_ms % 100 == 0`）。
//
// ★ 本层不碰 UART、不碰 UI、不碰 data_service：它只回答"现在是什么状态"，
//   按 §8 L10/L11 的裁决，**"冻结最后值 + 表情退常态"和"回退 Sim"由 UI/数据层
//   看着这个状态去做**（协议层不做决定）。
//
// ★ 契约没写、由本文件定下来的两个参数（§4 只说"滑动最小 + 一阶低通"，
//   没给窗口长度与低通系数），改它们不影响帧格式，只影响锁定速度：
//     · kOffsetWindow     = 64 个 tick（50 Hz 下 ≈1.28 s 的滑窗）
//     · kOffsetAlphaShift = 3   ⇒ 一阶低通 alpha = 1/8
//   于是估计值的固定点落在窗口最小值 ±4 ms 以内（整数低通的固有量化，
//   见 alphaRound() 的注释）—— 远小于 §4 那条"两板估计偏差 < 40 ms"的目标。
// ============================================================

namespace link {

// ---- §3 TICK 行的三档 / §4（数字是契约原文，别在别处抄） ----
static const uint32_t kTickPeriodMs  = 20u;     // 50 Hz（20 ms）
static const uint32_t kBasisLostMs   = 100u;    // >100 ms（5 个周期）没见 ⇒ 时间基准失效
static const uint32_t kDegradeMs     = 500u;    // >500 ms ⇒ 链路降级（L10：冻结最后值 + 表情退常态）
static const uint32_t kSimFallbackMs = 3000u;   // >3 s ⇒ 回退 Sim（沿用 data_service 的 3 秒规则）
static const uint32_t kAnimGridMs    = 100u;    // §4 的扫表对齐栅格

// ---- 偏移估计的两个参数（★ 见文件头） ----
static const uint8_t kOffsetWindow     = 64u;
static const uint8_t kOffsetAlphaShift = 3u;    // alpha = 1/(1<<3) = 1/8

// 断链之后回来的**第一个**样本与旧估计差多少，就认定"对端换历元了"（重启过）。
// ★ 为什么必须有这一条：纯滑动最小有个已知弱点 —— 对端重启后历元差整个变了，
//   若新历元差**比旧的大**，窗口最小值会被旧样本"钉住"整整一个窗口（1.28 s），
//   而 §7 的失败模式 5 要求"主板重启 ⇒ 从板自动恢复"。有了它，恢复是**一眼**的事。
static const int32_t kEpochJumpMs = 250;

// 三级超时的状态（§3 的 TICK 行 + §7 的失败模式表）
enum class LinkTimeState : uint8_t {
  Locked = 0,     // 有基准：距上一帧 TICK ≤ 100 ms
  NoBasis,        // 100 ms < age ≤ 500 ms：时间基准失效，**回退本地时钟**
  Degraded,       // 500 ms < age ≤ 3 s：链路降级（L10：冻结最后值 + 表情退常态）
  SimFallback,    // age > 3 s，或本机开机以来一次 TICK 都没见过：**回退 Sim**
};

const char* linkTimeStateName(LinkTimeState s);   // ASCII，日志用

// 按"距上一帧多久"判状态（§3 的三档；边界是 `>` 而不是 `≥`：
// 恰好 100 ms **仍算有基准**，因为契约写的是">100 ms 没见"）。
// ever_seen=false（开机以来一帧都没见过）⇒ 一律 SimFallback ——
// 否则刚上电 50 ms 的从板会自称"有基准"。
LinkTimeState linkTimeStateForAge(uint32_t age_ms, bool ever_seen);

// 距下一个栅格还有多少毫秒（现在正好落在栅格上 ⇒ 0）。
// §4 的用法：从板 UI 就绪后等到下一个 `tick_ms % 100 == 0` 再启动扫表动画，
// 两屏扫表起点差 ≤ 一个 tick 周期（20 ms）。
uint32_t msUntilGrid(uint32_t now_ms, uint32_t grid_ms = kAnimGridMs);

// ------------------------------------------------------------
// 主板侧：TICK 的节奏与 seq
// ------------------------------------------------------------
class TickGen {
 public:
  // 从复位起算：第一次 due() 立刻发一帧（tick_ms = 那一次的 now_ms）。
  void reset(uint32_t now_ms);

  // 到点了吗？到点就填好 out 并返回 true（**每次调用最多发一帧**）。
  // ★ 主循环被 LVGL 拖慢时**不补发突发**：跳过错过的槽位，但栅格相位保持
  //   （`next += 20` 而不是 `next = now + 20`）。补发会让 seq 在一圈里跳好几格、
  //   也会把链路的突发占满 —— §1.3 要的是"单次很短、能随时被打断"。
  //   `tick_ms` 始终 = 调用方给的 now_ms（§4：主板**自己的**单调毫秒）。
  bool due(uint32_t now_ms, TickMsg* out);

  uint8_t  seq() const { return mSeq; }
  uint16_t ticksSent() const { return mSent; }

 private:
  uint32_t mNextMs = 0;
  uint8_t  mSeq    = 0;
  uint16_t mSent   = 0;
  bool     mStarted = false;
};

// ------------------------------------------------------------
// 从板侧：偏移估计 + 三级超时
//
// 用法（与 data_service 同一个套路）：主循环每圈
//     link_time.update(millis());          // 推进状态机与失效/恢复簿记
//     if (link_time.state() == LinkTimeState::Locked) 用 masterNowMs() 对齐动画
//   收到 TICK 时：link_time.onTick(tick, millis());
//   收到 DATA 时：link_time.onData(millis());   // §3 DATA 行的"同 TICK 那三档"
// ------------------------------------------------------------
class LinkTime {
 public:
  void reset();

  // 收到一帧 TICK（m 是解包好的载荷，local_ms = 收到那一刻的本机 millis()）
  void onTick(const TickMsg& m, uint32_t local_ms);
  // 收到一帧 DATA（只影响 DATA 的新鲜度；时间基准仍由 TICK 定）
  void onData(uint32_t local_ms);
  // 主循环每圈调：刷新年龄、推进状态机、做失效/恢复的簿记
  void update(uint32_t local_ms);

  LinkTimeState state() const { return mState; }
  LinkTimeState dataState() const { return mDataState; }
  bool haveBasis() const { return mState == LinkTimeState::Locked; }

  // 估计出来的 (tick_ms - local_ms)。**只在 haveBasis() 时可信** ——
  // 失基准后契约要求"回退本地时钟"，所以 masterNowMs() 那时返回 local。
  int32_t  offsetMs() const { return mOffset; }
  bool     offsetValid() const { return mHaveOffset; }

  // 估计出来的"主板现在几毫秒"：有基准 = 本地 + 偏移；否则 = 本地（§3 的"回退本地时钟"）。
  uint32_t masterNowMs() const;

  uint32_t tickAgeMs() const { return mNowMs - mLastTickMs; }
  uint32_t dataAgeMs() const { return mNowMs - mLastDataMs; }
  bool     tickSeen() const { return mSeenTick; }

  uint16_t ticksSeen() const { return mTicks; }
  // seq 跳变的次数与按 seq 差值累计的丢帧数（§3：`TICK.seq` 让"回绕/丢帧可见"）
  uint16_t seqGaps() const { return mSeqGaps; }
  uint16_t seqMissing() const { return mSeqMissing; }
  uint8_t  lastSeq() const { return mLastSeq; }
  // 滑动窗口里当前的最小 d（没有样本时返回 0）—— 诊断用
  int32_t  windowMinMs() const;
  uint8_t  windowSamples() const { return mWinCount; }

 private:
  void   pushSample(int32_t d);
  void   clearWindow();
  void   refreshStates();

  int32_t  mWin[kOffsetWindow] = {0};
  uint8_t  mWinCount = 0;
  uint8_t  mWinHead  = 0;

  int32_t  mOffset     = 0;
  bool     mHaveOffset = false;

  uint32_t mNowMs      = 0;
  uint32_t mLastTickMs = 0;
  uint32_t mLastDataMs = 0;
  bool     mSeenTick   = false;
  bool     mSeenData   = false;

  uint16_t mTicks      = 0;
  uint16_t mSeqGaps    = 0;
  uint16_t mSeqMissing = 0;
  uint8_t  mLastSeq    = 0;
  bool     mHasSeq     = false;

  LinkTimeState mState     = LinkTimeState::SimFallback;
  LinkTimeState mDataState = LinkTimeState::SimFallback;
};

}  // namespace link

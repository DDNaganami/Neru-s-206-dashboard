// 双板链路协议 v1 —— 时基实现（§4）。纯逻辑：不碰 Arduino / 寄存器 / 堆。
#include "link_time.h"

namespace dashlink {

namespace {

// 一阶低通用整数：alpha = 1/(1<<shift)，**四舍五入**（不是截断）。
// ★ 为什么必须是四舍五入：截断在负方向上不对称（C++ 的整数除法向 0 取整），
//   于是估计值会带一个"朝 0 偏"的系统偏差，而且固定点不是一个点而是一段区间。
//   用四舍五入后固定点是 |mn - offset| ≤ 2^(shift-1) = 4 ms —— 这就是
//   本文件头上说的"固定点落在窗口最小值 ±4 ms 以内"的来历。
inline int32_t alphaRound(int32_t delta, uint8_t shift) {
  const int32_t half = (int32_t)1 << (shift - 1u);
  if (delta >= 0) return (delta + half) >> shift;
  return -((-delta + half) >> shift);
}

}  // namespace

const char* linkTimeStateName(LinkTimeState s) {
  switch (s) {
    case LinkTimeState::Locked:      return "locked";
    case LinkTimeState::NoBasis:     return "no_basis";
    case LinkTimeState::Degraded:    return "degraded";
    case LinkTimeState::SimFallback: return "sim";
    default:                         return "?";
  }
}

LinkTimeState linkTimeStateForAge(uint32_t age_ms, bool ever_seen) {
  if (!ever_seen) return LinkTimeState::SimFallback;   // 开机以来一帧都没见过 ⇒ 自跑
  if (age_ms > kSimFallbackMs) return LinkTimeState::SimFallback;
  if (age_ms > kDegradeMs)     return LinkTimeState::Degraded;
  if (age_ms > kBasisLostMs)   return LinkTimeState::NoBasis;
  return LinkTimeState::Locked;
}

uint32_t msUntilGrid(uint32_t now_ms, uint32_t grid_ms) {
  if (grid_ms == 0u) return 0u;
  const uint32_t rem = now_ms % grid_ms;
  return rem == 0u ? 0u : (grid_ms - rem);
}

// ------------------------------------------------------------
// TickGen
// ------------------------------------------------------------
void TickGen::reset(uint32_t now_ms) {
  mNextMs  = now_ms;
  mSeq     = 0;
  mSent    = 0;
  mStarted = true;
}

bool TickGen::due(uint32_t now_ms, TickMsg* out) {
  if (out == nullptr) return false;
  if (!mStarted) {            // 没 reset 也能用：第一圈就是起点
    mStarted = true;
    mNextMs  = now_ms;
  }
  // 有符号差 ⇒ millis() 回绕天然正确（与 §4 的 d 同一个套路）
  if ((int32_t)(now_ms - mNextMs) < 0) return false;

  out->tick_ms = now_ms;      // §4：主板自己的单调毫秒，不是合成出来的栅格时间
  out->seq     = mSeq;
  ++mSeq;                     // u8 自己回绕（255 → 0）
  ++mSent;

  // 跳过错过的槽位，但**保持栅格相位**（每次都 +20，不是 now+20）
  do {
    mNextMs += kTickPeriodMs;
  } while ((int32_t)(now_ms - mNextMs) >= 0);
  return true;
}

// ------------------------------------------------------------
// LinkTime
// ------------------------------------------------------------
void LinkTime::reset() {
  *this = LinkTime();
}

void LinkTime::clearWindow() {
  mWinCount = 0;
  mWinHead  = 0;
}

void LinkTime::pushSample(int32_t d) {
  if (mWinCount < kOffsetWindow) {
    mWin[mWinCount] = d;
    ++mWinCount;
    return;
  }
  mWin[mWinHead] = d;
  mWinHead = (uint8_t)((mWinHead + 1u) % kOffsetWindow);
}

int32_t LinkTime::windowMinMs() const {
  if (mWinCount == 0) return 0;
  int32_t mn = mWin[0];
  for (uint8_t i = 1; i < mWinCount; ++i) {
    if (mWin[i] < mn) mn = mWin[i];
  }
  return mn;
}

void LinkTime::onTick(const TickMsg& m, uint32_t local_ms) {
  // 这一拍**之前**是不是有基准 —— 断链后的"重锁"判据要用它
  const bool wasLocked = (mState == LinkTimeState::Locked);

  // seq 的跳变/回绕（§3：丢帧由它可见）。环形差：255 → 0 的差是 1，不算丢。
  if (mHasSeq) {
    const uint8_t step = (uint8_t)(m.seq - mLastSeq);
    if (step != 1u) {
      ++mSeqGaps;
      // step == 0 是"同一帧又来了/乱序"，没有"缺几个"可言 ⇒ 只计跳变
      if (step > 1u) mSeqMissing = (uint16_t)(mSeqMissing + (uint16_t)(step - 1u));
    }
  }
  mHasSeq  = true;
  mLastSeq = m.seq;
  if (mTicks < 0xFFFFu) ++mTicks;

  mLastTickMs = local_ms;
  mSeenTick   = true;
  mNowMs      = local_ms;     // 刚收到，年龄为 0

  // 偏移样本：d = tick_ms - local_ms（有符号减法天然处理 32 位回绕，§4）
  const int32_t d = (int32_t)(m.tick_ms - local_ms);
  pushSample(d);

  const int32_t mn = windowMinMs();
  if (!mHaveOffset) {
    mOffset     = mn;         // 第一次（或刚从 Sim 回退里恢复）：直接锁到最小值
    mHaveOffset = true;
  } else if (!wasLocked && (mn - mOffset > kEpochJumpMs || mOffset - mn > kEpochJumpMs)) {
    // 失基准期间回来的第一个样本与旧估计差得离谱 ⇒ 对端重启了（millis() 从 0 起算）。
    // 直接重锁，不让低通从旧值慢慢爬（否则两屏会错位一秒多）。
    mOffset = mn;
  } else {
    mOffset += alphaRound(mn - mOffset, kOffsetAlphaShift);
  }
  refreshStates();
}

void LinkTime::onData(uint32_t local_ms) {
  mLastDataMs = local_ms;
  mSeenData   = true;
  mNowMs      = local_ms;
  refreshStates();
}

void LinkTime::refreshStates() {
  const LinkTimeState prev = mState;
  mState     = linkTimeStateForAge(tickAgeMs(), mSeenTick);
  mDataState = linkTimeStateForAge(dataAgeMs(), mSeenData);

  if (prev == mState) return;
  if (mState == LinkTimeState::SimFallback) {
    // 回退 Sim：对端可能已经重启（millis() 从 0 开始 ⇒ 偏移整个变了），
    // 所以**丢掉估计**，下一个 TICK 从头锁（§7 失败模式 5：主板重启
    // ⇒ 主板起来后 HELLO/TICK 自动恢复，无需重启从板）。
    clearWindow();
    mHaveOffset = false;
  } else if (mState == LinkTimeState::NoBasis || mState == LinkTimeState::Degraded) {
    // 失基准：滑窗清空（"时间基准失效"就该重新攒），但保留低通状态 ——
    // 单纯断一下（几十~几百毫秒）时两个 millis() 的历元都没变，
    // 保留估计能让恢复瞬间就对齐，不必重新收敛 1.28 s。
    clearWindow();
  }
}

void LinkTime::update(uint32_t local_ms) {
  mNowMs = local_ms;
  refreshStates();
}

uint32_t LinkTime::masterNowMs() const {
  if (mState != LinkTimeState::Locked || !mHaveOffset) return mNowMs;   // 回退本地时钟
  return (uint32_t)(mNowMs + (uint32_t)mOffset);
}

}  // namespace dashlink

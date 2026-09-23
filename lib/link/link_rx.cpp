// 双板链路协议 v1 —— 接收侧实现（§2 的重同步 + §5 的角色冲突）。
// 纯逻辑：不碰 Arduino/寄存器/堆；缓冲是成员数组，没有一次动态分配。
#include "link_rx.h"

namespace dashlink {

void LinkRx::reset() {
  mLen = 0;
  mNeed = 0;
  mInCandidate = false;
  mStats = LinkRxStats();
  mVerMismatch = false;
  mRoleConflict = false;
  // mLocalRole 不重置：它是编译期角色的运行时快照，不是"状态"
}

void LinkRx::push(uint8_t b) {
  // 不可达分支（LEN ≤ 64 ⇒ 候选帧最多 71 B < 128 B），保留只为"永远不越界"。
  if (mLen >= kParseBufBytes) {
    ++mStats.noise_bytes;
    return;
  }
  mBuf[mLen++] = b;
}

void LinkRx::consume(uint16_t k) {
  if (k == 0u) return;
  if (k >= mLen) {
    mLen = 0;
    return;
  }
  for (uint16_t i = 0; i + k < mLen; ++i) mBuf[i] = mBuf[i + k];
  mLen = (uint16_t)(mLen - k);
}

void LinkRx::countDrop(DecodeErr e) {
  switch (e) {
    case DecodeErr::CrcError:      ++mStats.crc_err; break;
    case DecodeErr::LenOutOfRange: ++mStats.bad_len; break;
    case DecodeErr::BadSync:       ++mStats.bad_sync; break;
    case DecodeErr::LenMismatch:   ++mStats.len_mismatch; break;
    case DecodeErr::UnknownType:   ++mStats.unknown_type; break;
    default:                       ++mStats.bad_len; break;   // ShortFrame 不该走到这里
  }
}

LinkRx::Step LinkRx::feed(uint8_t b, Frame* out, DecodeErr* err) {
  push(b);
  return advance(out, err);
}

LinkRx::Step LinkRx::advance(Frame* out, DecodeErr* err) {
  // 一次调用里可能连着丢好几帧（重同步），把最近一次"丢"记下来：
  // 如果最后解出了帧，就报 Frame（丢弃已经进计数了）；如果只能等更多字节，
  // 就把那次丢弃报出去，让调用方知道"刚才有一帧没过"。
  bool     haveDrop = false;
  Step     dropStep = Step::Reject;
  DecodeErr dropErr = DecodeErr::Ok;

  for (;;) {
    if (!mInCandidate) {
      // ---- 猎手：找 SYNC（§2 的分流：回放行首字节 'V' 与 0x5A 不撞）----
      uint16_t j = 0;
      while (j < mLen && mBuf[j] != kSync) ++j;
      if (j > 0u) {
        mStats.noise_bytes += j;
        consume(j);
      }
      if (mLen == 0u) break;                 // 等更多字节
      mInCandidate = true;
      mNeed = 0;
    }

    // ---- 攒帧头：拿到 LEN 才知道要收多少 ----
    if (mNeed == 0u) {
      if (mLen < (uint16_t)(kOffLen + 1u)) break;
      const uint8_t len = mBuf[kOffLen];
      if (len > kLenNoWaitAbove) {
        // §2：>64 按坏帧丢，**不等载荷**（连 CRC 都不算）
        ++mStats.bad_len;
        haveDrop = true; dropStep = Step::Reject; dropErr = DecodeErr::LenOutOfRange;
        consume(1u);                         // 从 SYNC 之后一个字节继续找
        mInCandidate = false;
        continue;
      }
      mNeed = frameBytesForLen(len);
    }
    if (mLen < mNeed) break;                 // 这一帧还没收齐

    // ---- 收齐了：帧层判据 ----
    Frame f;
    const DecodeErr e = decodeFrame(mBuf, mNeed, &f);
    if (e == DecodeErr::Ok) {
      if (roleConflict(f.role, mLocalRole)) {
        // §5 ①：角色冲突 ⇒ 丢帧 + 报警（本层只计数/置标志，日志由调用方打）
        ++mStats.role_conflict;
        mRoleConflict = true;
        haveDrop = true; dropStep = Step::RoleDrop; dropErr = DecodeErr::Ok;
        consume(mNeed);
        mInCandidate = false;
        continue;                            // 剩下的字节接着找（可能还有好帧）
      }
      ++mStats.frames_ok;
      if (f.ver_mismatch) mVerMismatch = true;   // §2：只告警，不丢帧
      consume(mNeed);
      mInCandidate = false;
      if (out != nullptr) *out = f;
      if (err != nullptr) *err = DecodeErr::Ok;
      return Step::Frame;
    }

    // CRC 不过 / LEN 越界 / 未知 TYPE：丢这一帧并计数，再从 SYNC+1 继续找
    countDrop(e);
    haveDrop = true; dropStep = Step::Reject; dropErr = e;
    consume(1u);
    mInCandidate = false;
  }

  if (haveDrop) {
    if (err != nullptr) *err = dropErr;
    return dropStep;
  }
  if (err != nullptr) *err = DecodeErr::Ok;
  return Step::NeedMore;
}

bool LinkRx::poll(LinkPhy& phy, Frame* out, uint16_t max_bytes) {
  if (out == nullptr) return false;
  uint16_t budget = max_bytes;
  for (;;) {
    // 先把缓冲里已经够一帧的解出来（feed() 早退留下的字节都在这里）
    Frame f;
    if (advance(&f, nullptr) == Step::Frame) {
      *out = f;
      return true;
    }
    if (budget == 0u) return false;          // 本次预算用完（§1.3：单次要短）
    if (!phy.online()) return false;         // 断开：不读
    const int b = phy.read();
    if (b < 0) return false;                 // 现在没有字节（非阻塞契约）
    --budget;
    push((uint8_t)b);
  }
}

}  // namespace dashlink

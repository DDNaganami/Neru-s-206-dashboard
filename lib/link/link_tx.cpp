// 双板链路协议 v1 —— 发送侧实现（§1.2）。纯逻辑：只碰内存，写 PHY 的只有 pump()。
#include "link_tx.h"

namespace link {

void LinkTx::reset() {
  mTail    = 0;
  mCount   = 0;
  mDropped = 0;
  mSent    = 0;
}

bool LinkTx::enqueue(const uint8_t* frame, uint16_t n) {
  if (frame == nullptr || n == 0u) {
    ++mDropped;
    return false;
  }
  // ★ 整帧判空间：不够就**整帧丢**，绝不让半帧进环（§1.2 ②）。
  //   注意 n 可能比整个环还大 —— 那条路径同样落在这里（freeBytes() ≤ kRingBytes）。
  if (n > freeBytes()) {
    ++mDropped;
    return false;
  }
  uint16_t pos = (uint16_t)((mTail + mCount) % kRingBytes);   // 写位置 = 读位置 + 已有字节数
  for (uint16_t i = 0; i < n; ++i) {
    mBuf[pos] = frame[i];
    pos = (uint16_t)((pos + 1u) % kRingBytes);
  }
  mCount = (uint16_t)(mCount + n);
  return true;
}

bool LinkTx::enqueueFrame(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t role) {
  uint8_t buf[kFrameBytesMax];
  const uint16_t n = encodeFrame(type, payload, len, role, buf, sizeof(buf));
  if (n == 0u) {          // 参数非法：也算丢一帧，免得"发不出去"悄无声息
    ++mDropped;
    return false;
  }
  return enqueue(buf, n);
}

uint16_t LinkTx::pump(LinkPhy& phy) {
  if (!phy.online()) return 0;                 // 断开：数据留在环里，等链路回来
  const int room = phy.availableForWrite();
  if (room <= 0) return 0;                     // 塞不进：留着，下一圈再来（**不等待**）

  uint16_t want = (uint16_t)room;
  if (want > mCount) want = mCount;
  if (want == 0u) return 0;

  // 环里的一段可能绕回 ⇒ 最多两次 write。**不重试**：一次 pump 就是一次尝试，
  // 单次很短、可随时被中断（§1.3）。
  const uint16_t first = (uint16_t)((mTail + want <= kRingBytes) ? want
                                                                 : (kRingBytes - mTail));
  size_t wrote = phy.write(mBuf + mTail, (size_t)first);
  if (wrote > (size_t)first) wrote = (size_t)first;            // 防御：PHY 返回超额
  uint16_t written = (uint16_t)wrote;
  if (written == first && want > first) {
    const uint16_t rest = (uint16_t)(want - first);
    size_t wrote2 = phy.write(mBuf, (size_t)rest);
    if (wrote2 > (size_t)rest) wrote2 = (size_t)rest;
    written = (uint16_t)(written + (uint16_t)wrote2);
  }

  mTail  = (uint16_t)((mTail + written) % kRingBytes);
  mCount = (uint16_t)(mCount - written);
  mSent += written;
  return written;
}

}  // namespace link

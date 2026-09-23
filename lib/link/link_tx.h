#pragma once
#include <stdint.h>

#include "link_frame.h"
#include "link_phy.h"

// ============================================================
// 双板链路协议 v1 —— **发送侧**（§1.2 的三条硬约束）
//
// 形状就是契约写的那一条：链路帧先**编码**进一个**自有环形缓冲**（建议 ≥512 B），
// 再由 UART 的硬件 FIFO/DMA 排出；入队前判空间，**不够就丢这一帧**。
//
// ★ 为什么必须这样（先例是实测，不是推测，见 §1.2）：115200 只有 ≈11.5 KB/s，
//   总线超过 ~230 帧/秒时 `Serial0.write` 会**阻塞**、把主循环一起拖住 ⇒
//   丢 VAN 边沿 → 坏帧变多，看起来像"FCS 约定不对"。链路帧绝不能那样发。
//
// ★ 三条纪律（每一条都有用例钉住）：
//   ① **不在 ISR/回调里发**：enqueue()/enqueueFrame() 只碰内存，
//      **一个字节都不写 PHY**；写 PHY 的只有 pump()，而 pump() 只许在主循环里调。
//      （本层没有 ISR 概念、也没有临界区：单生产者 + 单消费者，head/count 两个索引
//      就够 —— 这也是"不在中断里发"能成立的前提。）
//   ② **整帧进出**：空间不够就丢**整帧**，绝不把半帧写进环（半帧比丢帧糟：
//      对端会一直等一个永远不来的尾巴）。丢弃计数，且由 `TICK.seq` 与从板
//      `frames_ok` 在链路上可见（§1.2 ②）。
//   ③ **pump() 永不忙等**：只写 `availableForWrite()` 允许的那些字节；
//      写不完就留在环里，下一圈接着来；PHY 断开时一个字节都不动。
// ============================================================

namespace dashlink {

class LinkTx {
 public:
  // §1.2 ② 的建议值：≥512 B。最坏突发（DATA 13 + TICK 12 + STATUS 23 +
  // HELLO 12 + EVENT 11 = 71 B）在空环里必然放得下（有用例钉住）。
  static const uint16_t kRingBytes = 512u;

  void reset();

  // 入队一个**已编码**的整帧。true = 已进环；
  // false = 空间不够，**整帧丢掉**并计数（绝不写半帧、绝不等待）。
  bool enqueue(const uint8_t* frame, uint16_t n);

  // 便利：编码 + 入队（编码失败 —— 参数非法 —— 同样算丢一帧）。
  bool enqueueFrame(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t role);

  // 主循环里调：把环里的字节推给 PHY，返回本次推出去的字节数。
  // 单次调用最多做两次 phy.write（环可能绕回），**不会重试、不会等待**。
  uint16_t pump(LinkPhy& phy);

  uint16_t queued() const { return mCount; }
  uint16_t freeBytes() const { return (uint16_t)(kRingBytes - mCount); }
  uint32_t droppedFrames() const { return mDropped; }
  uint32_t sentBytes() const { return mSent; }

 private:
  uint8_t  mBuf[kRingBytes] = {0};
  uint16_t mTail  = 0;      // 读位置（pump 从这里取）
  uint16_t mCount = 0;      // 环里现有字节数
  uint32_t mDropped = 0;
  uint32_t mSent    = 0;
};

}  // namespace dashlink

#pragma once
#include <stddef.h>
#include <stdint.h>

#include <deque>

#include "link_phy.h"

// ============================================================
// **可编程假 PHY**（内存环回 + 故障注入）—— 测试专用，不进固件。
//
// 存在的理由：§1.2 那三条硬约束（非阻塞、整帧丢、不在 ISR 里发）与 §2 的重同步
// 全是"行为"性质，只有把 PHY 变成可编程的，才能在宿主机上把它们**测出来**：
//   · 两个假 PHY 一对接就是一条内存链路：a.write() 的字节进 b 的读缓冲；
//   · `maxWriteChunk` 模拟"硬件 FIFO 只吃这么几个字节"⇒ 逼出分片；
//   · `dropEveryNth` 模拟线路上丢字节 ⇒ 逼出 crc_err 与重同步；
//   · `latencySteps` + step() 模拟"写进去要过一会儿才到对端"（虚拟时间）；
//   · `up = false` 模拟拔线/没接线 ⇒ 逼出"链路不在时不许忙等、不许丢数据"。
//
// ★ 语义上要说清的两条（不然用例会写成"自欺欺人"）：
//   ① `write()` 返回的是"**被硬件收下**的字节数" —— 被 dropEveryNth 丢掉的字节
//      也算收下了（真 UART 也是这样：字节进了 FIFO，发送方无从得知它在线上没了）。
//   ② 对端 `up == false` 时，本端 write() 照样成功（本地 FIFO 收下了），
//      字节记进 `lostBytes` —— 这正是"对端掉电、本端还不知道"的样子。
// ============================================================
class FakeLinkPhy : public dashlink::LinkPhy {
 public:
  // ---- 接线 ----
  void connect(FakeLinkPhy* peer) { mPeer = peer; }

  // ---- 可编程故障（默认全是"理想线路"）----
  uint8_t txCapacity    = 64;    // availableForWrite() 的上限（模拟硬件 FIFO 深度）
  uint8_t maxWriteChunk = 0;     // 每次 write 最多吸收几个字节（0 = 不限）⇒ 分片
  uint8_t dropEveryNth  = 0;     // 每第 N 个字节丢一个（0 = 不丢）⇒ 丢字节
  uint8_t latencySteps  = 0;     // 写进去先压 N 个 step() 才对对端可见 ⇒ 延迟
  bool    up            = true;  // false = 断开：read 返回 -1、write 不收、availableForWrite = 0

  // ---- 计数（用例的判据）----
  uint32_t writeCalls   = 0;     // write() 被调了几次（"有没有忙等"看它）
  uint32_t wroteBytes   = 0;     // 被收下的字节数
  uint32_t droppedBytes = 0;     // 注入丢掉的字节数
  uint32_t lostBytes    = 0;     // 对端不在/未接线而丢掉的字节数

  // ---- dashlink::LinkPhy ----
  int available() override { return up ? (int)mRx.size() : 0; }

  int read() override {
    if (!up || mRx.empty()) return -1;
    const int b = mRx.front();
    mRx.pop_front();
    return b;
  }

  int availableForWrite() override { return up ? (int)txCapacity : 0; }

  size_t write(const uint8_t* data, size_t n) override;

  bool online() const override { return up; }

  // ---- 虚拟时间：把延迟队列里的字节往前推一格 ----
  void step();

  // ---- 测试夹具：直接往本端读缓冲塞字节 ----
  void feed(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; ++i) mRx.push_back(data[i]);
  }
  size_t rxBytes() const { return mRx.size(); }
  size_t delayedBytes() const { return mDelay.size(); }

  void clearCounters() {
    writeCalls = wroteBytes = droppedBytes = lostBytes = 0;
    mByteSeq = 0;
  }

 private:
  struct Pending {
    uint8_t b;
    uint8_t left;
  };

  void deliver(uint8_t b) {
    if (mPeer != nullptr && mPeer->up) {
      mPeer->mRx.push_back(b);
    } else {
      ++lostBytes;
    }
  }

  std::deque<uint8_t> mRx;        // 对端写进来、等本端读的字节
  std::deque<Pending> mDelay;     // 延迟中的字节
  FakeLinkPhy* mPeer = nullptr;
  uint32_t mByteSeq = 0;          // dropEveryNth 的计数
};

inline size_t FakeLinkPhy::write(const uint8_t* data, size_t n) {
  ++writeCalls;
  if (!up || data == nullptr || n == 0) return 0;

  size_t cap = (size_t)txCapacity;                       // 硬件 FIFO 深度
  if (n > cap) n = cap;
  if (maxWriteChunk != 0 && n > (size_t)maxWriteChunk) {  // 分片
    n = (size_t)maxWriteChunk;
  }

  size_t accepted = 0;
  for (size_t i = 0; i < n; ++i) {
    ++mByteSeq;
    if (dropEveryNth != 0 && (mByteSeq % (uint32_t)dropEveryNth) == 0u) {
      ++droppedBytes;      // 收下了,但在线上没了(见文件头 ①)
      ++accepted;
      continue;
    }
    if (latencySteps == 0) {
      deliver(data[i]);
    } else {
      Pending p;
      p.b = data[i];
      p.left = latencySteps;
      mDelay.push_back(p);
    }
    ++accepted;
  }
  wroteBytes += (uint32_t)accepted;
  return accepted;
}

inline void FakeLinkPhy::step() {
  const size_t n = mDelay.size();
  for (size_t i = 0; i < n; ++i) {
    Pending p = mDelay.front();
    mDelay.pop_front();
    if (p.left > 0) --p.left;
    if (p.left == 0) {
      deliver(p.b);
    } else {
      mDelay.push_back(p);
    }
  }
}

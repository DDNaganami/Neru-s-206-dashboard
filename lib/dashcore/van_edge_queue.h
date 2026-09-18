#pragma once
#include <stdint.h>

// ============================================================
// VAN 边沿队列 —— 硬件中断(ISR)与解码器之间的那一段
//
// 为什么需要它(不能直接在 ISR 里解码吗):
//   解码器(van_phy_wire)要跑 4B5B 展开 + E-Manchester 判槽 + CRC-15,
//   一帧几百个边沿、每个边沿几十微秒 —— 放进 ISR 会:
//     ① 边沿期间关中断,后面的边沿直接丢(125kbps / 8µs 一个槽,丢一个就废);
//     ② 把 WiFi / LVGL 的时序一起拖垮。
//   所以 ISR 里只做"记时间戳 + 存电平"这件几个微秒的事,主循环排空队列。
//
// 为什么不用临界区/互斥:
//   单生产者(ISR)**只改 head**、单消费者(tick)**只改 tail** ——
//   各自只读对方那一个变量,天然无锁。前提是别在别处动这两个下标。
//   (代价:队列满了会丢新的边沿。丢了那一帧的 CRC 会不过,但**不会卡死**,
//    而且 dropped() 会把次数报出来 —— 实车调线时它是"ISR 跟不跟得上"的唯一证据。)
//
// 时间戳单位是**微秒**(与 esp_timer_get_time()/micros() 同源):
//   van_wire 内部按 8µs = 1 TS 换算。
// ============================================================
class VanEdgeQueue {
public:
  // 一帧约几百个边沿(SOF + 若干字节,每字节 4B5B/E-Manchester 之后 ~20 个沿),
  // 2048 能兜住连续几帧的突发;S3 上这点 DRAM(8KB)不算什么。
  static const uint16_t kCapacity = 2048;

  struct Edge {
    uint32_t t_us;
    uint8_t  level;   // 1 = recessive(高),0 = dominant(低)
  };

  // 只由生产者(ISR)调用。满了丢这一个边沿并计数。
  void push(uint32_t t_us, uint8_t level) {
    const uint16_t next = (uint16_t)((head_ + 1u) % kCapacity);
    if (next == tail_) {          // 满:消费者还没跟上
      ++dropped_;
      return;
    }
    buf_[head_].t_us = t_us;
    buf_[head_].level = level ? 1 : 0;
    head_ = next;                 // ★ 最后才动 head:消费者看到新 head 时数据已就位
    ++pushed_;
  }

  // 只由消费者(主循环)调用:取出一个边沿。空了返回 false。
  bool pop(Edge* out) {
    if (tail_ == head_) return false;
    if (out) *out = buf_[tail_];
    tail_ = (uint16_t)((tail_ + 1u) % kCapacity);
    return true;
  }

  // 队列里还有多少待处理的边沿(head - tail,环形)
  uint16_t size() const {
    return (uint16_t)((head_ + kCapacity - tail_) % kCapacity);
  }
  bool empty() const { return head_ == tail_; }

  uint32_t pushed() const { return pushed_; }
  uint32_t dropped() const { return dropped_; }

  // 复位(切换数据源/重新开始抓帧时用;不要在 ISR 正在跑的时候调)
  void reset() {
    head_ = tail_ = 0;
    pushed_ = dropped_ = 0;
  }

private:
  volatile uint16_t head_ = 0;    // 生产者写
  volatile uint16_t tail_ = 0;    // 消费者写
  volatile uint32_t pushed_ = 0;
  volatile uint32_t dropped_ = 0;
  Edge buf_[kCapacity] = {};
};

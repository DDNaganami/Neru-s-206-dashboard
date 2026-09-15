#pragma once
#include <stdint.h>
#include "van_phy.h"
#include "van_wire.h"

// ============================================================
// VAN 物理层的线路层实现:GPIO 边沿时间戳 → 解码 → VanPacket
//
// 分工:
//   van_wire.h  BitDecoder/FrameParser 负责 SOF、4B5B、CRC-15、帧尾判据
//   本文件      负责把硬件中断记下的边沿喂进去,并在帧完成时转成 VanPacket
//
// 硬件侧只需要做一件事:每次 RO 脚电平变化时,记下时间戳(µs),
// 调 onEdge()。不在这里访问任何寄存器,所以宿主机可以直接喂合成波形。
//
// 关键设计:字节走 BitDecoder 的 **回调**路径(不是队列),因为一个边沿
// 区间内可能同时含数据字节和帧尾(总线空闲时区间很长)。若等区间处理完
// 再 drain 队列,EOF 之后的字节会丢,帧永远解不出来。
//
// 时间戳单位是微秒(与 millis() 同源,便于和 rx_ms 对齐);
// van_wire 内部按 8µs = 1 TS 换算。
// ============================================================

class VanPhyWire : public VanPhy {
 public:
  void begin() override;

  // 非阻塞:当前实现没有硬件 FIFO,由中断直接调 onEdge,这里只处理超时
  void tick(uint32_t now_ms) override;

  // 喂一个电平变化。t_us = 该边沿的时间戳(µs,单调递增);
  // level = 变化后的电平(true = recessive/高)。返回本次是否解出一帧。
  bool onEdge(uint32_t t_us, bool level);

  // 统计(实车调线/确认信号质量用)
  struct Stats {
    uint32_t edges = 0;
    uint32_t frames = 0;
    uint32_t frames_fcs_ok = 0;
    uint32_t frames_dropped = 0;   // 队列溢出或帧太长
  };
  const Stats& stats() const { return stats_; }
  void resetStats() { stats_ = Stats(); }

 private:
  // 把 BitDecoder 回调的字节直接转交给 FrameParser。
  // 时间戳用"当前边沿"近似(帧内相邻字节的时间差远小于 rx_ms 精度)。
  class ByteRelay : public van::ByteSink {
   public:
    void onByte(uint8_t b) override {
      if (fp) fp->pushByte(b, now_ns, nullptr);
    }
    van::FrameParser* fp = nullptr;
    uint64_t now_ns = 0;
  };

  van::BitDecoder dec_;
  van::FrameParser fp_;
  ByteRelay relay_{};
  Stats stats_{};
};

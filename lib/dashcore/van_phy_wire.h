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
  // level = 变化后的电平(true = recessive/高)。
  // ★ 本函数**不关帧**,也**不负责武装解析器**:
  //   前者只在 finish();后者由 relay 的 onFrameStart() 在 SOF 命中那一刻完成
  //   (SOF 与后续字节可能同在一次 pushEdge 里,边沿返回后再判断就晚了)。
  bool onEdge(uint32_t t_us, bool level);

  // ★ 唯一负责关帧的入口:问解析器"手上有没有待收帧",有就 endFrame() 并报包。
  //   真实硬件里由"总线空闲超时"驱动;测试夹具里喂完最后一帧后显式调。
  //   返回是否真的收出一帧。
  bool finish();

  // 诊断:解码器相位/计数快照,以及"是否有待收帧"
  van::BitDecoder::Snap snap() const { return dec_.snap(); }
  bool framePending() const { return fp_.inFrame(); }
  uint16_t pendingBytes() const { return fp_.pendingBytes(); }

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
  // 把 BitDecoder 的回调转交给 FrameParser。
  // 时间戳用"当前边沿"近似(帧内相邻字节的时间差远小于 rx_ms 精度)。
  //
  // ★ onFrameStart() 是这套接线的关键:SOF 命中与后续数据字节可能落在
  //   同一次 pushEdge 里,所以"武装解析器"必须发生在**字节之前**,
  //   而不是等边沿返回后由 onEdge 判断(那样必然丢一头)。
  class ByteRelay : public van::ByteSink {
   public:
    void onFrameStart() override {
      if (fp) fp->beginFrame(now_ns);
      started = true;
    }
    void onByte(uint8_t b) override {
      // 只在已武装时才收:SOF 之前的总线噪声不进解析器。
      if (fp && fp->inFrame()) fp->pushByte(b, now_ns, nullptr);
    }
    van::FrameParser* fp = nullptr;
    uint64_t now_ns = 0;
    bool started = false;          // 本帧是否已收到 onFrameStart(诊断用)
  };

  van::BitDecoder dec_;
  van::FrameParser fp_;
  ByteRelay relay_{};
  Stats stats_{};
  uint64_t last_edge_ns_ = 0;      // finish() 用它当帧结束时间
  bool frame_started_ = false;     // 解码器报过 InFrame(诊断/测试用)
};

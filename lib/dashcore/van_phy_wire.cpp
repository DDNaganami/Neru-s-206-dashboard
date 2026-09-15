#include "van_phy_wire.h"

void VanPhyWire::begin() {
  dec_.reset();
  fp_.reset();
  relay_.fp = &fp_;
  relay_.now_ns = 0;
  dec_.setByteSink(&relay_);   // 字节走回调:边沿区间内的字节一个不丢
}

bool VanPhyWire::onEdge(uint32_t t_us, bool level) {
  ++stats_.edges;

  const uint64_t t_ns = (uint64_t)t_us * 1000ull;
  relay_.now_ns = t_ns;        // 回调时用它给字节打时间戳
  last_edge_ns_ = t_ns;        // finish() 收尾时用它当帧结束时间

  const van::BitDecoder::Ev ev = dec_.pushEdge(t_ns, level);
  if (dec_.overflowed()) ++stats_.frames_dropped;

  // ★ 帧的收尾**只发生在 finish() 里**,onEdge 不再抢收。
  //
  // 起因(实测):onEdge 原先在 ev == EndOfFrame 时也会调 endFrame()。
  // 而夹具在帧体之后还会发一条"收尾"边沿,那条边沿正好触发 EndOfFrame ——
  // 于是帧在 finish() 之前就被收掉了,而且**那时解析器缓冲已经被消费成空**,
  // 事后无论怎么改 finish() 都救不回那些字节。
  // 症状就是 `finish 前: frames=1 pending=0 bytes=0`。
  //
  // 现在分工很清楚:
  //   onEdge()  只负责喂边沿(不再关帧)
  //   finish()  唯一负责收尾(问解析器有没有待收帧,有就 endFrame)
  frame_started_ = (dec_.snap().phase == van::FramePhase::InFrame);
  return false;
}

void VanPhyWire::tick(uint32_t now_ms) {
  (void)now_ms;
  // 线路层是纯数据驱动的,没有需要轮询的硬件状态。
  // 若将来改成 RMT/DMA 环形缓冲,在这里读 FIFO。
}

bool VanPhyWire::finish() {
  // ★ 判据以**解析器**为准(手上有没有待收帧),不看解码器相位:
  //   空闲超时那条路径会把解码器相位清成 Idle,但字节还在解析器缓冲里,
  //   那时候问解码器会得到"没有帧",字节就白丢了。
  if (!fp_.inFrame()) return false;

  // 让解码器把尾部半截丢掉并切到"帧已结束"(不在这里判帧界,只是对齐状态)
  dec_.finish();

  van::Frame f;
  const uint64_t t_ns = last_edge_ns_;
  const bool ok = fp_.endFrame(t_ns, &f, dec_.ackDominant());
  if (ok) {
    ++stats_.frames;
    if (f.fcs_ok) ++stats_.frames_fcs_ok;
    if (sink_) {
      VanPacket pkt{};
      van::frameToPacket(f, (uint32_t)(t_ns / 1000000ull), &pkt);
      sink_->onPacket(pkt);
    }
  } else {
    ++stats_.frames_dropped;
  }
  // 收尾完成后回到 Idle:finish() 是"关帧"的终点,下一帧从干净状态开始。
  // 注意必须用 markIdle() —— dec_.resync() 在这里不够:它也会清 mArmed,
  // 而 mArmed 一清,下一次 pushEdge 里的 phase 赋值又按"未武装"回落,
  // 结果 finish() 之后读到的 phase 仍是 Eof 而不是 Idle
  // (实测:断言 phase==Idle 报 Was 2)。
  dec_.markIdle();
  return ok;
}

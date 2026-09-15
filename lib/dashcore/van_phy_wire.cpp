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

  const van::BitDecoder::Ev ev = dec_.pushEdge(t_ns, level);
  if (dec_.overflowed()) ++stats_.frames_dropped;

  // 收尾判据只有两个,都必须是**可靠**的:
  //   1) 缓冲里已经凑出一个 FCS 校验通过的完整帧 —— 这是唯一真正可靠的
  //      "帧已完整"信号。"8 个连续 recessive"不能当帧尾用:帧内合法数据
  //      本身就可能带出 8 个连续 recessive(实测 8A 22 5A 这帧的帧体里
  //      就有一段),照它收尾会在帧中途把帧截断。
  //   2) 总线进入空闲(解码器报 EndOfFrame):此时无论缓冲里有没有完整帧,
  //      都要收尾并复位解码器相位,好接下一帧。
  const bool idle = (ev == van::BitDecoder::Ev::EndOfFrame);
  if (!fp_.hasCompleteFrame() && !idle) return false;

  van::Frame f;
  const bool ok = fp_.endFrame(t_ns, &f, dec_.ackDominant());
  if (ok) {
    // 只有真正解析出帧才计入统计。
    // 一帧物理帧可能触发两次收尾(先"凑出完整帧"、后"总线空闲"),
    // 第二次缓冲已清空会失败 —— 计进去会让 frames 虚高(实测 1 帧报 2)。
    ++stats_.frames;
    if (f.fcs_ok) ++stats_.frames_fcs_ok;
    if (sink_) {
      VanPacket pkt{};
      van::frameToPacket(f, t_us / 1000u, &pkt);
      sink_->onPacket(pkt);
    }
  }
  return ok;
}

void VanPhyWire::tick(uint32_t now_ms) {
  (void)now_ms;
  // 线路层是纯数据驱动的,没有需要轮询的硬件状态。
  // 若将来改成 RMT/DMA 环形缓冲,在这里读 FIFO。
}

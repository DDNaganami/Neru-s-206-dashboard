#pragma once
#include <stdint.h>
#include "van_source.h"

// VAN 物理层接口:把总线字节流解成 VanPacket 喂给 VanSource::onPacket()。
// 实驱动(SN65HVD230 + VanBus 库,或 RMT 收发)在硬件到货后实现;
// 当前用 VanPhyStub 占位,保证 main() 里的集成点结构完整,换实驱动不动数据层。
class VanPhy {
public:
  virtual ~VanPhy() = default;
  virtual void begin() = 0;
  virtual void tick(uint32_t now_ms) = 0;   // 非阻塞:读硬件 FIFO、解帧、回调 sink
  void setSink(VanSource* sink) { sink_ = sink; }

protected:
  VanSource* sink_ = nullptr;
};

// 无硬件时的占位实现:什么都不做,假数据链路不受影响
class VanPhyStub : public VanPhy {
public:
  void begin() override {}
  void tick(uint32_t now_ms) override { (void)now_ms; }
};

// 实驱动的样子(保留作实现参考):
//   class VanPhyRmt : public VanPhy {
//     void begin() override {
//       // 引脚见 PINOUT.md(VAN RX=16 / TX=15 / DE-RE=GND 监听模式)
//       // RMT 初始化:VAN 为 125kbps、曼彻斯特编码,VanBus 库或 RMT 收发包
//     }
//     void tick(uint32_t now_ms) override {
//       // 读硬件环形缓冲 → 校验/解帧 → VanPacket p{};
//       // p.iden/len/data 填好, p.rx_ms = now_ms;
//       // if (sink_) sink_->onPacket(p);
//     }
//   };

#pragma once
#include <stdint.h>
#include "van_source.h"

// 帧的接收方。物理层只依赖这个接口,不依赖具体数据源
// (方便单测接记录用的 sink,也方便将来接别的消费者)。
class VanSink {
public:
  virtual ~VanSink() = default;
  virtual void onPacket(const VanPacket& pkt) = 0;
};

class VanPhy {
public:
  virtual ~VanPhy() = default;
  virtual void begin() = 0;
  virtual void tick(uint32_t now_ms) = 0;   // 非阻塞:读硬件 FIFO、解帧、回调 sink
  void setSink(VanSink* sink) { sink_ = sink; }

protected:
  VanSink* sink_ = nullptr;
};

// 数据源版本:把 VanSource 当作 sink 用(实车接线最省事的一条路)
class VanSourceSink : public VanSink {
public:
  explicit VanSourceSink(VanSource* s) : src_(s) {}
  void onPacket(const VanPacket& pkt) override {
    if (src_) src_->onPacket(pkt);
  }

private:
  VanSource* src_;
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
//       // RMT 初始化:VAN 为 125kbps、E-Manchester(4B5B)编码
//     }
//     void tick(uint32_t now_ms) override {
//       // 读硬件环形缓冲 → 取边沿时间戳 → 交给 van_phy_wire 的 VanPhyWire
//     }
//   };

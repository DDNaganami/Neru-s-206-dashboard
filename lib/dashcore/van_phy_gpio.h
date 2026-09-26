#pragma once
#include "van_phy.h"

// ============================================================
// VAN 物理层的**硬件胶水**:RO 引脚的电平变化 → VanPhyWire 解码器
//
// 分工(和 van_phy_wire.h 里写的一样,这里只是把边沿"从哪来"补上):
//   GPIO 中断(本文件)  读电平 + 读时间戳 → 塞进 VanEdgeQueue(几个微秒)
//   tick(本文件)        排空队列 → VanPhyWire::onEdge();总线空闲 → finish()
//   解码(van_phy_wire) 4B5B / E-Manchester / CRC-15 → VanPacket
//
// 接线(PINOUT.md 第一段;★ 脚号 2026-09-27 晚改定):
//   SN65HVD230:VCC=3.3V、GND=GND、RO=**GPIO44**(2.8C 的 12PIN 第 3 针 `RXD`)、
//     D(TX/DI)=3V3(★ 不能悬空:低电平=显性=会主动干扰总线)、RS=GND(只收)
//     ★ 旧写法的 `RO=GPIO16` **已作废**:16 电气上自由但**不在 12PIN 排针上**
//       (要焊飞线);`GPIO15` 更不行(板载 I2C)。理由与现场前提(不插 UART Type-C)
//       见 `van_phy_gpio.cpp` 的 `VAN_RX_PIN` 那一段。
//     · RS 是第 8 脚(不是 DE/RE):接 GND=高速模式,接 VCC=待机(一帧都收不到);
//       6 脚模块(3.3V GND RX TX CANH CANL)没有这一脚 —— 模块内部已接 GND,无需外接(PINOUT.md「抓帧盒」)
//   ★ 模块上的 120Ω 终端电阻必须拆掉(总线两端才有终端,我们挂在中间)
//
// ★ 只有设备端编译(整个头/实现被 ARDUINO 包住):
//   它依赖 attachInterrupt / esp_timer_get_time,宿主机没有这些。
//   能宿主机测的那一半在 van_edge_queue.h(纯逻辑)与 van_phy_wire.cpp(解码),
//   所以"中断到不了"这件事只影响真实硬件,不影响可测性。
// ============================================================
#if defined(ARDUINO)

#include "van_edge_queue.h"
#include "van_phy_wire.h"

class VanPhyGpio : public VanPhy {
public:
  void begin() override;
  void tick(uint32_t now_ms) override;

  // ★★ 必须转发给内嵌的 wire_(2026-09-22 修)。
  //
  // 为什么:VanPhyGpio 继承 VanPhy,内嵌的 VanPhyWire **也**继承 VanPhy ——
  // 于是内存里有**两份** sink_。基类那个非虚 setSink 只会写 VanPhyGpio 自己那份,
  // 而真正解帧的 VanPhyWire::finish() 读的是 wire_ 那份 ⇒ 永远 nullptr。
  //
  // 实测症状(定位它花了很久,因为太有迷惑性):
  //   物理层诊断行 `van: edges=… frames=458 fcs_ok=458` **照常涨** ——
  //   因为 ++stats_.frames 在 `if (sink_)` **之前**,计数不受影响;
  //   但 `VAN %03X` 帧行一行不打、`SRC speed` 永远是 sim、嗅探计数恒为 0。
  //   而 van_replay 那条路是直接调 g_data.vanSource().onPacket()、绕开物理层,
  //   所以回放能出 `SRC speed=van` —— 把这个问题盖住了。
  void setSink(VanSink* sink) override {
    VanPhy::setSink(sink);        // 保留基类那份(接口语义完整)
    wire_.setSink(sink);          // ★ 真正生效的那一份
  }

  // 供 GPIO 中断调用(实现见 .cpp 的静态跳板)。必须是 public:
  // 跳板是文件级函数,够不到私有成员。
  void onIsrEdge();

  // 诊断:实车调线时看的几个数
  const VanPhyWire& wire() const { return wire_; }
  const VanEdgeQueue& queue() const { return q_; }
  uint32_t framesEmitted() const { return frames_emitted_; }

private:
  VanPhyWire wire_;
  VanEdgeQueue q_;
  uint32_t last_edge_us_ = 0;
  uint32_t frames_emitted_ = 0;
  uint32_t last_report_ms_ = 0;
};

#endif  // ARDUINO

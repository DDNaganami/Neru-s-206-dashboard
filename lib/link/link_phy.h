#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================
// 双板链路协议 v1 —— **物理层抽象**（§1「物理层与发送侧约束」）
//
// 契约要的是一条"可替换的字节通道"：真实实现是 ESP32 的 UART0 那一对脚
// （43/44，115200 8N1，§0/§1.1），**本轮不碰任何引脚/外设**（上板阶段才做）。
// 所以这里只有接口 + 纪律；测试里配一个可编程的假 PHY（test/test_dashcore/
// fake_link_phy.h：内存环回 + 丢字节/分片/延迟/断开）。
//
// ★ 四条纪律（都是硬约束，违反了不会有编译期信号）：
//   ① **全部非阻塞**：available()/read()/write() 一个都不许等。写不完就少写几个，
//      由调用方（LinkTx::pump）下一圈接着来。
//   ② **只有主循环写**：ISR（VAN 那边是 `van_isr_thunk`/`onIsrEdge`）只做
//      "读电平 + 读时间戳 + 入队"。链路帧的发送一律在主循环里（§1.2 ①）。
//   ③ **不得在 VAN 关帧窗口附近做长操作**：一次 pump() 只写
//      availableForWrite() 允许的那些字节，单次很短、可随时被打断（§1.3）。
//   ④ 不从 VAN 帧回调里发：数据帧的内容取自主循环里的快照（§1.2 ③）——
//      这条由调用方保证，本层只提供"先入自有环形缓冲、再由主循环排出"的形状。
// ============================================================

namespace dashlink {

class LinkPhy {
 public:
  virtual ~LinkPhy() {}

  // 现在可读的字节数（非阻塞；0 = 现在没有）。断开时返回 0。
  virtual int available() = 0;

  // 读一个字节；-1 = 现在没有（**不是**错误）。断开时恒返回 -1。
  virtual int read() = 0;

  // 现在还能塞进多少字节（非阻塞；0 = 塞不进，调用方保留数据等下一圈）。
  virtual int availableForWrite() = 0;

  // 写 n 个字节，返回**实际写进去的**字节数（0..n，允许少于 n）。
  // ★ 只写能写的那些：绝不忙等、绝不 delay()（§1.2 ②）。
  virtual size_t write(const uint8_t* data, size_t n) = 0;

  // 这条链路现在还在不在（false = 拔线/断开：不读不写）。
  // 默认 true；假 PHY 用它模拟"从板没接线"。
  virtual bool online() const { return true; }
};

}  // namespace dashlink

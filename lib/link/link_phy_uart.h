#pragma once

// ============================================================
// 双板链路协议 v1 —— **真实 UART 的 LinkPhy**（§0/§1：115200 8N1，GPIO43/44）
//
// 契约：ARCHITECTURE.md「## 双板链路协议 v1 范围」
//   · §0 引脚表：主板 **GPIO43** 发 → 从板 **GPIO44** 收；从板 **GPIO43** 发 → 主板
//     **GPIO44** 收 ⇒ 两端同一套脚：**43 = TX、44 = RX**（本文件按这个默认值）。
//   · §1.1 波特率 **115200 8N1**。
//   · §1.2 发送侧三条硬约束：① 不在 ISR 里发；② 自有环 + 不等待、
//     **不够就丢这一帧**；③ 从快照发、不从回调发。本文件只负责 ①②的"下半个"
//     （PHY 那一侧），③ 由调用方（link_app / main）保证。
//
// ★★ 为什么 TX 侧必须自带环形缓冲（这是本文件存在的全部理由）
//
//   ESP32 Arduino 核心里的 `HardwareSerial`：**默认 tx ring buffer 是 0**
//   （`HardwareSerial.cpp` 构造：`_txBufferSize(0)`），而 IDF 的 `uart_write_bytes()`
//   在那时的语义是（`driver/uart.h` 原文）：
//       "If the UART driver's parameter 'tx_buffer_size' is set to zero:
//        This function will **not return until** all the data have been sent out,
//        or at least pushed into TX FIFO."
//   ⇒ `Serial0.write(buf, n)` 在 FIFO 满时**阻塞**，直到这 n 个字节都推进 FIFO。
//   这正是 §1.2 引的那次实测教训（PINOUT「已知的坑」）：115200 只有 ≈11.5 KB/s，
//   日志把 FIFO 塞满 ⇒ `Serial0.write` 把主循环一起拖住 ⇒ **丢 VAN 边沿 ⇒ 坏帧变多**，
//   症状看上去像"FCS 约定不对"。
//
//   所以本类**不把 `Serial*.write()` 当非阻塞 API 用**：
//     ① 出方向另开一个环（kTxRingBytes），`write()` 只**拷贝进环**，一个字节都不落
//        UART；环满 ⇒ 返回 0，由调用方（LinkTx::pump → 下一圈再试）决定怎么办。
//     ② 真正碰 UART 的只有 `pumpTx()`（**主循环里**调，与 §1.2 ① 一致），
//        它先把 FIFO/驱动里能塞的塞进去，再把环里剩下的搬进去 —— 每一步都有上界，
//        单次调用很短、可随时被打断（§1.3）。
//     ③ `availableForWrite()` = `min(硬件可写量, 环剩余, 保留水位)`。tx buffer 为 0 时
//        `uartAvailableForWrite()` 报的是 **FIFO 空闲格数（128）** —— 那是"写进去不会
//        阻塞"的**边界**（写等于它就可能撞上阻塞），所以再留 kTxFifoHeadroom 个字节，
//        让"返回 N ⇒ 写 N 个字节不会阻塞"这条**严格成立**。
//   ★ 另一条同样安全的写法是 `setTxBufferSize(512)` 把硬件环打开（那样 `write()` 只
//     拷进驱动环就返回）。本文件**故意不依赖它**：驱动环满了 `uart_write_bytes` 一样
//     会等，还是要靠"够了才写"这条纪律；而环开在**我们自己的**代码里，
//     丢帧、水位、统计全在同一处看得见。
//
// ★★ 链路 UART 必须**独立于日志口**（§0 的待办 —— **2026-09-23 已做掉**）
//   §0「载体」那条：链路用的就是 UART0 那一对脚（43/44）⇒ 显示构建必须**关掉 UART0
//   文本日志**，日志只走原生 USB-CDC，否则 `dash_logf()` 的文本会**混进链路数据流**。
//   收口的地方是 `lib/dashcore/dash_log.h` 的 **`DASH_LOG_UART0`**：定义了
//   `LINK_PHY_UART`（= 这份固件里有链路 PHY）时它默认 **0** ⇒ `dash_log_begin()` 不开
//   `Serial0`、`dash_logf()` 不写 `Serial0`，日志只走 USB-CDC。
//   ★ 于是"链路与日志同用一个外设"这件事**在编译期就不可能悄悄发生**：真有人显式
//     `-DDASH_LOG_UART0=1` 把这个危险组合拼回来，`link_phy_uart.cpp` 里那道
//     `#error` 会当场拦住（它判的正是 `LINK_UART_PORT == 0 && DASH_LOG_UART0`）。
//   ⇒ 上板时撞上的是**编译错误**，不是静默的坏帧。
//
// ★★ 回环模式（LINK_PHY_LOOPBACK，默认 0 = 关）
//   目的：**单块裸 S3** 上做端到端验证（把 TX/RX 两个空闲脚短接，见
//   docs/LINK-LOOPBACK.md）—— 不需要第二块板、不需要改接线之外的任何东西。
//   为什么**不能**用 43/44 回环：那两个脚在裸 S3 devkit 上接着**板载 USB-串口桥**，
//   短接会把桥的 TX 一起并进回路（一个推挽输出对我们一个推挽输出 = 对打），
//   而 §8 L1 记录的那颗 `FSUSB42UMX` 模拟开关是"二选一"、**摘不掉**它。
//   ⇒ 回环默认走 **UART1 的 GPIO17/18**（裸 N16R8 上就是 17/18 两个脚）。
//   ★ UART0 的 43/44 **在硬件上也做不了本机回环**：那两块板子的 CH340 与 FSUSB42
//     把 43/44 占了，"桥"那一侧永远在回路上 —— 所以回环**必须是另一个 UART**。
//
// ★★ 那道闸门落在 **link_phy_uart.cpp**（不是本头文件）
//   "链路占 UART0 ⇒ 日志不许也写 UART0" 这条要成立，判据是"**这份固件有没有把日志
//   放在 UART0 上**"—— 那件事的**唯一出处**是 `dash_log.h` 的 `DASH_LOG_UART0`，
//   而"链路 PHY 被链进固件"这件事的唯一证据是那个翻译单元（它同时 include 了
//   `dash_log.h` 与链路口径）。所以：
//     · 本头文件（谁都能 include）**只给常量与纪律**，不替别人下结论；
//     · 闸门在 .cpp：链路 PHY 一进固件、而 UART0 上**还**挂着日志 ⇒ **编译期直接报错**，
//       并写明两条出路（去掉那个显式覆盖 / 把链路挪到别的 UART）。
//   ★ 2026-09-23：本条待办已做掉（`DASH_LOG_UART0`，见上一段的两条 ★★）。
//
// ★ 本文件只在**固件构建**里编译（`LINK_PHY_UART` 由 platformio.ini 定义）；
//   宿主机（native / pcpreview）拿不到 `HardwareSerial` 的可用与可写 API，
//   编了也只会炸。接线常量与编译期守卫在 `link_phy_pins.h`（纯头文件，宿主机可测）。
// ============================================================

#include "link_phy_pins.h"

#if LINK_PHY_UART

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "link_phy.h"

namespace dashlink {

class LinkPhyUart : public LinkPhy {
 public:
  // ---- 引脚 / 端口（默认按契约 §0 的"谁都是 43 发、44 收"）----
  // 数字的唯一出处是 link_phy_pins.h（编译期宏 + 守卫都在那里），这里只是换个
  // 好记的名字给调用方用。
  static const int8_t  kDefaultTxPin = kLinkTxPin;      // 43
  static const int8_t  kDefaultRxPin = kLinkRxPin;      // 44
  // 链路 UART 端口号：0 = UART0（= 契约 §0 说的 43/44 那对脚）。
  // ★ 与日志口的关系见文件头"链路 UART 必须独立于日志口"。
  static const int8_t  kDefaultPort  = kLinkUartPort;

  // ---- 回环模式（LINK_PHY_LOOPBACK=1 时）----
  static const int8_t  kLoopPort    = kLoopbackUartPort;   // 1 = UART1
  static const int8_t  kLoopTxPin   = kLoopbackTxPinC;     // 17
  static const int8_t  kLoopRxPin   = kLoopbackRxPinC;     // 18

  // ---- 缓冲尺寸（§1.2 ② 的"自有环 ≥512 B"是 LinkTx 那一个；这里是 PHY 自己的）----
  // 出方向这一环只要**装得下 worst case 单次 pump** 就够（LinkTx 那个 512 B 环才是
  // 契约点名的那一个）：DATA 13 B / STATUS 23 B ⇒ 256 B 富余。它**不是**为了让写
  // 永不失败，而是为了"write() 永不阻塞"。所以环满就丢，绝不扩容、绝不等待。
  static const uint16_t kTxRingBytes = 256u;
  // 入方向：一次 pump 最多从 PHY 拉 LinkRx::poll 的默认预算（64 B），256 B 给
  // "主循环一圈没来"留余量。真溢出了由 rxOverflow() 计数看见（UART0 的驱动环也是
  // 默认 256 B，比本环不小 ⇒ 真要丢也是先丢在驱动那一侧，那条路我们数不到）。
  static const uint16_t kRxRingBytes = 256u;
  // 见文件头 ③：`availableForWrite()` 报数与"写这么多不会阻塞"之间留的余量。
  static const uint16_t kTxFifoHeadroom = 8u;

  LinkPhyUart() {}

  // 初始化。loopback=true ⇒ 用 **UART1 + GPIO17/18**（本机回环，见文件头）；
  // false ⇒ 用 UART0 + GPIO43/44（契约 §0）。
  // ★ 只在 setup()/主循环里调（会开中断），绝不在 ISR 上下文里调。
  void begin(bool loopback = false);

  // 真正的写 UART：**只在主循环里调**（§1.2 ①）。返回本次搬进 UART 的字节数。
  // 单次调用有上界（≤ kTxFifoHeadroom + 硬件可写量），不忙等、不 delay。
  uint16_t pumpTx();

  // ---- dashlink::LinkPhy ----
  int    available() override;
  int    read() override;
  int    availableForWrite() override;
  size_t write(const uint8_t* data, size_t n) override;
  bool   online() const override { return mOnline; }

  // ---- 自检 / 用例判据 ----
  uint32_t rxTotal() const { return mRxTotal; }       // 从 UART 读进来的总字节数
  uint32_t rxOverflow() const { return mRxOverflow; } // 入环时没地方放而丢掉的字节
  uint32_t txTotal() const { return mTxTotal; }       // 真正推进 UART 的总字节数
  uint32_t txOverflow() const { return mTxOverflow; } // write() 时环满而丢掉的字节
  uint16_t txPending() const { return mTxCount; }     // 还在 PHY 环里等 pumpTx 的字节
  bool     started() const { return mOnline; }
  int8_t   port() const { return mPort; }
  int8_t   txPin() const { return mTxPin; }
  int8_t   rxPin() const { return mRxPin; }
  bool     loopback() const { return mLoopback; }
  HardwareSerial* serial() { return mSerial; }

 private:
  void start(HardwareSerial* s, int8_t port, int8_t tx, int8_t rx, bool loopback);
  int  hwTxRoom() const;

  HardwareSerial* mSerial = nullptr;
  int8_t mPort = kDefaultPort;
  int8_t mTxPin = kDefaultTxPin;
  int8_t mRxPin = kDefaultRxPin;
  bool   mLoopback = false;
  bool   mOnline = false;

  // 出方向（write → pumpTx → UART）
  uint8_t  mTxBuf[kTxRingBytes] = {0};
  uint16_t mTxHead = 0;
  uint16_t mTxCount = 0;
  uint32_t mTxTotal = 0;
  uint32_t mTxOverflow = 0;

  // 入方向（UART → read）
  uint8_t  mRxBuf[kRxRingBytes] = {0};
  uint16_t mRxHead = 0;
  uint16_t mRxTail = 0;
  // ★ 这两个由 UART 的 RX 任务（ISR 上下文）写、主循环读 ⇒ volatile。
  //   其余成员只由主循环碰，不需要。
  volatile uint32_t mRxTotal = 0;
  volatile uint32_t mRxOverflow = 0;
};

}  // namespace dashlink

#endif  // LINK_PHY_UART

#pragma once

// ============================================================
// 双板链路协议 v1 —— **空壳 PHY**（"这份固件里没有链路硬件"那一档）
//
// 用途只有一处：`src/main.cpp` 的从板侧。**两个角色现在都带真 PHY**
// （见 main.cpp 的 `#if defined(LINK_PHY_UART)` 那一段），所以本类只在
// "角色是从板、但这份固件没有编链路 PHY"的构建里被选到 —— 今天的例子是
// **pcpreview**（宿主机预览，`LINK_ROLE` 默认 0、没有 `LINK_PHY_UART`）
// 与经典 ESP32 的 `[env:esp32dev]`（同上，而且那边连 `Serial0` 都不存在）。
//
// 语义（契约 §1.2 ②）：`availableForWrite()==0` ⇒ 上层（`LinkTx::pump`）
// **一个字节都不写**、也不会忙等；`read()` 立刻返回 -1 ⇒ 非阻塞契约成立。
// 于是"没有链路硬件"这件事在数据面上表现为"链路静默"，而不是"卡住主循环"。
//
// ★★ 为什么它还要有 `begin()` / `pumpTx()` / `txPin()` / `rxPin()` / `port()`
//   —— 这五个都不是 `LinkPhy` 接口上的东西，但**必须有**：
//   调用方（`src/main.cpp`）**两个角色共用一份代码**（`g_link_phy` 一个实例），
//   而"这份固件有没有真 UART"是**编译期**的事（`LINK_PHY_UART`）。要是让调用方
//   去 `#if` 分叉出两套写法，那"两个角色形状相同"这条设计就没了 —— 而那正是
//   本类要避免的（分叉的地方越多，"从板少接了一行"这种事就越容易悄悄发生）。
//   ⇒ 于是本类把同一组形状**补齐**，全部是安全空操作：
//       · `begin()` → 什么都不做（不是错误：这份固件本来就没有链路硬件）；
//       · `pumpTx()` → 返回 0；
//       · 三个访问器 → 报**契约 §0 的编译期常量**（43/44/UART0），
//         这样"开机那一行日志"在两个角色/两种 PHY 上打印的都是同一句话
//         （不撒谎：那一行说的是"链路该怎么接"，不是"这个类接上了"）。
//
// ★ 为什么它是**纯头文件、无 Arduino 依赖**：宿主机（native / pcpreview）也要编
//   它 —— 与 `link_phy_pins.h` 同一条理由（那边连 `HardwareSerial` 都没有）。
// ★ 它**不假装**在线（`online()==false`）：谁能读到 `online()` 就该看见"没接线"。
// ============================================================

#include "link_phy.h"
#include "link_phy_pins.h"

namespace dashlink {

class LinkPhyNull : public LinkPhy {
 public:
  // 与 LinkPhyUart 同名同义的空操作（见文件头那段"为什么还要有这五个"）。
  void begin(bool = false) {}
  // ★★ 2026-09-27 晚修：这里原来写的是 `pumpTx()`（**没有参数**），而 `main.cpp`
  //   在 ESP-NOW 那一档改成 `pumpTx(now)` 之后，**所有非 ESP-NOW 的构建都编不过**：
  //     src/main.cpp:2200: error: too many arguments to function call, expected 0, have 1
  //   受影响的正是"没有无线"的那几档：`pcpreview`（宿主机预览）、`esp32s3-rgb`
  //   （显示档，`LINK_PHY_UART=0` ⇒ 空壳）、以及抓帧盒/回环那几档里的空壳分支。
  //   ⇒ 参数补上并给默认值，**形状与 `LinkPhyEspNow::pumpTx(uint32_t now_ms = 0u)`
  //     一致**（注释里原来就写着"UART / 空壳那两档的 pumpTx() 参数有默认值"——
  //     那句话当时是**愿望**，不是事实；现在它才是事实）。
  //   ★ 空壳不需要知道时刻（它没有射频计数器），参数只为同形。
  uint16_t pumpTx(uint32_t now_ms = 0u) { (void)now_ms; return 0u; }

  int8_t txPin() const { return kLinkTxPin; }      // 43（契约 §0 的编译期常量）
  int8_t rxPin() const { return kLinkRxPin; }      // 44
  int8_t port() const { return kLinkUartPort; }    // 0 = UART0

  // ---- dashlink::LinkPhy ----
  int available() override { return 0; }
  int read() override { return -1; }              // 非阻塞契约：没有就是 -1
  int availableForWrite() override { return 0; }  // 0 ⇒ 上层一个字节都不写（§1.2 ②）
  size_t write(const uint8_t*, size_t) override { return 0; }
  bool online() const override { return false; }  // 没接线 ⇒ 不读不写
};

}  // namespace dashlink

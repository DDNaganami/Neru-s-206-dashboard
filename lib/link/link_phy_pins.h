#pragma once
#include <stdint.h>

// ============================================================
// 双板链路协议 v1 —— **接线常量与编译期守卫**（纯头文件，无 Arduino 依赖）
//
// 单独拆出来的理由有两个，都不是为了好看：
//   ① 真 UART 的类（link_phy_uart.h）在**宿主机上编不了**（宿主机没有
//      `HardwareSerial::availableForWrite` / `write`），但"哪个脚是 TX、哪个是 RX、
//      两端是不是同一套脚、回环用的是不是 43/44"这些**接线事实**必须能在 native
//      上被用例钉住（接线错了在板上不会有任何编译期信号，只会"发出去没人收"）。
//   ② §0 的引脚口径散在文档里（"谁都是 43 发、44 收"），这里给出**唯一一份**
//      代码里的定义 —— 固件与用例都从这里取，别再各抄一遍数字。
//
// 契约出处：ARCHITECTURE.md §0 的引脚表 +「★ 本节对「接线定案」的一处新增」：
//   | 方向         | 主板脚        | 从板脚        |
//   | A → B（主流量）| GPIO43 输出   | GPIO44 输入   |
//   | B → A（次流量）| GPIO44 输入   | GPIO43 输出   |
//   ⇒ **每块板都是"43 发、44 收"**，编译期不分叉：`LINK_TX_PIN` / `LINK_RX_PIN`
//     的默认值对两个角色是同一套。
//   ★★ 2026-09-27 晚更正：本行原来写"VAN RX 特意避开 43/44（走 GPIO15）"——
//     那**两句都作废**：15 是这块 2.8C 的板载 I2C（TCA9554/QMI8658/PCF85063 共用，
//     拿它收 VAN 会和扩展器打架），而 VAN RX 现在**就走 GPIO44**
//     （12PIN 排针上只有 43/44 能当自由脚，见 `platformio.ini` 的 master-now 与 `PINOUT.md`）。
//   这两件事**不冲突**的原因：**无线那一档（`LINK_PHY_UART=0`）根本不碰 43/44**；
//   而"有线档 + VAN 收在 43/44"那个危险组合（两个推挽输出对顶）由
//   `lib/dashcore/van_phy_gpio.cpp` 里一道 `#error` 在**编译期**拦住。
// ============================================================

#ifndef LINK_UART_PORT
// 链路所在的 UART 端口号。★ 按契约 §0「载体」那条，链路用的就是 **UART0 那一对脚**
// （43/44）⇒ 0。★ 回环模式**不靠改这一条**，它另开一个端口常量
// （LINK_LOOPBACK_UART_PORT = 1），见 link_phy_uart.h 的 begin()。
#define LINK_UART_PORT 0
#endif

// ============================================================
// ★★ `LINK_PHY_UART` —— "这份固件里到底有没有真实的链路 PHY"（**数值口径**）
//
// 取值：**1 = 有**（真 `LinkPhyUart`：UART0 + 43/44 + 115200）/ **0 = 没有**（空壳）。
// 由 `platformio.ini` 的 env 给：`[env:esp32s3]` 及其抓帧盒/回环 env 是 1，
// `[env:esp32s3-rgb]` 显式 0（它的 43/44 要留给 UART0 文本日志），
// 两个角色镜像在继承来的那串之后各写一条 1 把它拿回来。
//
// ★★ 为什么这里要给**默认 0**，而且所有判据都写成 `#if LINK_PHY_UART`
//    （**不是** `#if defined(LINK_PHY_UART)`）—— 2026-09-25 实测踩到的一课：
//      · `platformio.ini` 里用 `-U` 撤销父 env 的 `-D` **不生效**：`-U` 与
//        `-D` 挤在 `build_flags = ${env:<父>.build_flags}` 那一串里时，
//        PlatformIO 解析 build_flags 会把 `-U` **丢掉**（`-D` 照收）
//        ⇒ `[env:esp32s3-rgb]` 曾经"看起来撤掉了、其实一直是 1"。
//      · 现在那条改成显式 `-DLINK_PHY_UART=0`（`-D` 的合并语义是**后者覆盖前者**，
//        实测可靠）。而"定义了但为 0"必须与"根本没定义"**同义** ——
//        否则一个 `defined()` 就又把 0 当成"有 PHY"了。
//   ⇒ 于是本文件给出这个默认值，且**全仓库统一用 `#if LINK_PHY_UART`**：
//     `lib/link/link_phy_uart.h` / `link_phy_uart.cpp` / `link_phy_null.h`、
//     `lib/dashcore/dash_log.h`、`src/main.cpp`。
//     `#if` 对"没定义"的宏按 0 处理 ⇒ 没定义也不会编错，语义与 =0 一致。
// ============================================================
#ifndef LINK_PHY_UART
#define LINK_PHY_UART 0
#endif

// 数值口径的编译期收口：这个宏只认 0/1（写了别的值一定是 -D 写错了）。
#if (LINK_PHY_UART != 0) && (LINK_PHY_UART != 1)
#error "LINK_PHY_UART 只能是 0(没有真 PHY) 或 1(有真 PHY) —— 别的值一定是 -D 写错了"
#endif

#ifndef LINK_TX_PIN
// 主板 GPIO43（UART 口 `TXD` / 12PIN `TXD`）→ 从板 RX
#define LINK_TX_PIN 43
#endif

#ifndef LINK_RX_PIN
// 从板 GPIO44（UART 口 `RXD` / 12PIN `RXD`）← 主板 TX
#define LINK_RX_PIN 44
#endif

#ifndef LINK_PHY_LOOPBACK
// 单板回环模式：默认**关**。参考值：打开后本机 TX/RX 短接即可收发
// （接线与期望输出见 docs/LINK-LOOPBACK.md）。
// ★ 本宏只影响**用哪一组脚**（下面的 LINK_LOOPBACK_* 常量），
//   不改变 LINK_UART_PORT —— 链路本身的接线口径永远是 43/44。
#define LINK_PHY_LOOPBACK 0
#endif

#ifndef LINK_LOOPBACK_UART_PORT
// ★ 回环的收发**必须换一个 UART**，不能还留在 UART0 上：
//   UART0 的 43/44 在裸 S3 devkit 上接着板载 USB-串口桥（CH340/CH343P），
//   而 §8 L1 记录的那颗 `FSUSB42UMX` 是"二选一"的模拟开关 —— 它**摘不掉**桥，
//   短接 43/44 就等于把桥的推挽 TX 一起并进回路（对打），回环结果不可信。
//   ⇒ UART0 上**硬件**就做不了本机回环，这不是软件选择。
#define LINK_LOOPBACK_UART_PORT 1
#endif

#ifndef LINK_LOOPBACK_TX_PIN
// UART1 的 TX 脚（裸 N16R8 / S3-DevKitC-1 上就是 GPIO17）
#define LINK_LOOPBACK_TX_PIN 17
#endif

#ifndef LINK_LOOPBACK_RX_PIN
// UART1 的 RX 脚（同上，GPIO18）
#define LINK_LOOPBACK_RX_PIN 18
#endif

#ifndef LINK_PHY_UART_ALLOW_LOG_ON_UART0
// ★ **已退役（2026-09-23）**：这条宏是"链路占了 UART0、而日志也写 UART0"的**豁免**开关
//   —— `§0「载体」`那条待办做完之后它就**没有消费者**了。
//   现在那条判据的**唯一出处**是 `lib/dashcore/dash_log.h` 的 `DASH_LOG_UART0`
//   （有 `LINK_PHY_UART` 时默认 0 ⇒ 日志不写 UART0），闸门在 `link_phy_uart.cpp`：
//   真有人显式 `-DDASH_LOG_UART0=1`，那道 `#error` 会当场拦住。
//   ★ 这里保留一个**恒为 0** 的同名宏，只为一件事：万一还有旧文档/旧命令带着
//     `-DLINK_PHY_UART_ALLOW_LOG_ON_UART0=1`，它**不会再让闸门失效**
//     （闸门已经不读它了）。**别再给新 env 加这一条** —— 显示构建现在不需要任何豁免。
#define LINK_PHY_UART_ALLOW_LOG_ON_UART0 0
#endif

#ifndef LINK_BAUD
// §1.1：115200 8N1。别在别处再抄一遍数字。
#define LINK_BAUD 115200
#endif

// ---- 编译期守卫：把"接线错了"从"上板才发现"提前到"编译期" ----
// ★ 本文件（link_phy_pins.h）是**宿主机也编**的纯头文件：它的消费者除了固件，还有
//   native 用例（接线常量得能在宿主机上被钉住）。所以这里的守卫只用**宏 + 无条件
//   static_assert**，不依赖任何"谁 include 了我"的判断。
//   ★ "链路占 UART0 而日志也写 UART0" 那条**不在这里** —— 它需要知道"这份固件把日志
//     放在哪个 UART 上"，那是 `dash_log.h` 的 `DASH_LOG_UART0`，而判它的人是
//     link_phy_uart.cpp（同时 include dash_log.h 的那一个翻译单元）。见那个文件的闸门。
#if LINK_TX_PIN == LINK_RX_PIN
#error "LINK_TX_PIN 与 LINK_RX_PIN 不能是同一根脚：TX 是推挽输出、RX 是输入，同一根脚上两者互斥（契约 §0：43 发、44 收）"
#endif

// ★ 回环那三条约束的守卫：**故意不写成 `#if LINK_PHY_LOOPBACK`** ——
//   回环是编译期开关、平时关着，写成条件式守卫就等于"平时没人检查这三个常量"，
//   而那正是最容易改坏又没人发现的地方（要等某天真去回环时才发现，那时你会先
//   怀疑线、怀疑固件、怀疑板子）。所以这里用**无条件**的模板 static_assert：
//   任何翻译单元（含 native 用例）编到这儿都得过。同一组判据在用例里还有一份
//   可执行断言（test_link_phy_uart.cpp），两边互为"影子"。
namespace dashlink {
namespace pins_detail {

template <int Delay>
struct LoopbackWiringMustBeSane {
  static_assert(Delay == 0,
                "回环接线不成立：要求 ① 两个脚不同；② 都不是 GPIO43/44"
                "（裸 S3 devkit 上那两脚接着板载 CH340/CH343P 桥，短接会把桥的推挽 TX"
                "并进回路 —— 见 LINK_LOOPBACK_UART_PORT 那段）；"
                "③ 回环 UART 与链路 UART 不是同一个（UART0 的 43/44 摘不掉桥）。"
                "默认值 GPIO17/18 + UART1 满足这三条。");
};

// ★ 为什么要套一层模板 + 一次"必须被实例化"的调用：`static_assert(条件, ...)` 里的
//   条件必须是**常量表达式**，而上面那三个条件用的是宏（在这个头文件里当然是常量）——
//   但把它直接写在命名空间作用域也行。套模板是为了让"条件不成立"时的**报错位置**
//   落在这个模板的实例化点上，而不是每一个 include 本文件的地方都报一遍长串上下文。
template <int Delay>
inline void checkLoopbackWiring() {
  (void)sizeof(LoopbackWiringMustBeSane<Delay>);
}

}  // namespace pins_detail

// 运行期可见的同一条判据（用例会断言它；模板那条是编译期版本）。
static const bool kLoopbackWiringSane =
    (LINK_LOOPBACK_TX_PIN != LINK_LOOPBACK_RX_PIN) &&
    (LINK_LOOPBACK_TX_PIN != 43) && (LINK_LOOPBACK_TX_PIN != 44) &&
    (LINK_LOOPBACK_RX_PIN != 43) && (LINK_LOOPBACK_RX_PIN != 44) &&
    (LINK_LOOPBACK_UART_PORT != LINK_UART_PORT);

}  // namespace dashlink

namespace {
// include 本头文件就编译一次上面那个检查 ⇒ 三个常量被改坏时**编译期**就炸。
// （匿名命名空间、没有人调它：没有运行期开销，也不会 ODR 冲突。）
inline void linkPhyPinsSelfCheck() {
  dashlink::pins_detail::checkLoopbackWiring<0>();
}
}  // namespace

// ---- 运行期可见的常量（用例读它们，别再抄数字）----
namespace dashlink {

// §1.1 的波特率：115200 8N1
static const uint32_t kLinkBaud = (uint32_t)LINK_BAUD;

// 契约 §0 的默认接线：**谁都是"43 发、44 收"**
static const int8_t kLinkTxPin   = (int8_t)LINK_TX_PIN;      // 43
static const int8_t kLinkRxPin   = (int8_t)LINK_RX_PIN;      // 44
static const int8_t kLinkUartPort = (int8_t)LINK_UART_PORT;  // 0 = UART0

// 回环模式
static const bool   kLoopbackEnabled = (LINK_PHY_LOOPBACK != 0);
static const int8_t kLoopbackUartPort = (int8_t)LINK_LOOPBACK_UART_PORT;  // 1 = UART1
static const int8_t kLoopbackTxPinC   = (int8_t)LINK_LOOPBACK_TX_PIN;     // 17
static const int8_t kLoopbackRxPinC   = (int8_t)LINK_LOOPBACK_RX_PIN;     // 18

// ★ 一条"改坏了会静默"的口径，写成常量让用例盯着：链路与回环**不是同一个 UART**。
static const bool kLoopbackUsesOwnUart = (kLoopbackUartPort != kLinkUartPort);

}  // namespace dashlink

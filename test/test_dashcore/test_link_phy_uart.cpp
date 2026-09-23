// 双板链路协议 v1 —— **真实 UART 那一侧的接线口径**用例（契约 §0 / §1.1）
//
// 为什么"接线"值得单开一组用例：接线错了在板上**没有任何编译期信号**，
// 症状只是"发出去没人收"或"收进来的全是噪声"—— 与"对端没上电""代码有 bug"
// 长得一模一样。而这几条口径（谁发谁收、哪个 UART、回环用哪两个脚）**全都写在
// 文档里**，代码里没有第二处会拦住它们。所以在这里钉死。
//
// ★ 本文件**只 include link_phy_pins.h**（纯头文件，宿主机编得了）：
//   LinkPhyUart 那个类在宿主机上编不了（没有 HardwareSerial::availableForWrite /
//   write）—— 而"类里的引脚默认值"就是从这个头文件取的，所以这里测到的常量
//   与固件用的**是同一份**（见 link_phy_uart.h 的 kDefaultTxPin = kLinkTxPin）。
//
// 本组用例覆盖：
//   · 契约 §0/§1.1 的字面值（43 发、44 收、UART0、115200）；
//   · "两块板同一套脚"（§0 的★：A→B 用 43→44，B→A 用 43→44）；
//   · 回环那三条约束（两个不同脚、都不是 43/44、另开一个 UART）——
//     它们**不依赖 LINK_PHY_LOOPBACK 是否打开**（见 pins 头文件里那段说明）。
#include <unity.h>

#include "link_phy_pins.h"
#include "link_role.h"

// ------------------------------------------------------------
// 契约 §0 的引脚表 / §1.1 的波特率
// ------------------------------------------------------------
static void test_link_pins_match_contract(void) {
  // §0：「主板 **GPIO43**（UART 口 TXD）→ 从板 **GPIO44**（UART 口 RXD）」
  TEST_ASSERT_EQUAL_INT(43, (int)link::kLinkTxPin);
  TEST_ASSERT_EQUAL_INT(44, (int)link::kLinkRxPin);
  // §0「载体」：链路用的就是 **UART0 那一对脚（43/44）**
  TEST_ASSERT_EQUAL_INT(0, (int)link::kLinkUartPort);
  // §1.1：**115200 8N1**
  TEST_ASSERT_EQUAL_UINT32(115200u, link::kLinkBaud);
}

// ★ §0 那条"本节对「接线定案」的一处新增"：v1 要双向跑，于是
//     A→B：主板 43 输出 → 从板 44 输入
//     B→A：主板 44 输入 ← 从板 43 输出
//   ⇒ **每块板都是"43 发、44 收"**，编译期**不分叉**。
//   这条要是被改成"按角色不同脚"，两块板的接线表就会分家（一个 43/44、一个 44/43），
//   而 4 线线缆只有一对 TX/RX ⇒ 必有一端接反。没有任何编译期信号会提醒。
static void test_link_pins_are_role_independent(void) {
  // 两个角色（§2 的 ROLE 字段只有这两位取值）读到的都是同一组常量 ——
  // 这条是"常量不随角色变"的**可执行**说法：本文件与 link_role.h 一起编译，
  // LINK_ROLE 取哪个值都不影响上面那两个断言。
  TEST_ASSERT_TRUE(link::kLocalRole == link::kRoleMaster || link::kLocalRole == link::kRoleSlave);
  TEST_ASSERT_NOT_EQUAL((int)link::kLinkTxPin, (int)link::kLinkRxPin);

  // 43/44 是板载 USB-串口桥（CH343P）那一对，VAN 特意避开它们走 GPIO15
  // （«本方案用途»表：GPIO15 = 主板 VAN 收发器 RX）⇒ 链路与 VAN 不许抢同一根脚。
  // ★ 这里只钉"链路这一侧"的 43/44；VAN 那根脚在 van_phy_gpio.cpp，
  //   由那条"特意避开 43/44"的口径与文档一起兜着。
  TEST_ASSERT_TRUE(link::kLinkTxPin == 43 || link::kLinkTxPin == 44);
  TEST_ASSERT_TRUE(link::kLinkRxPin == 43 || link::kLinkRxPin == 44);
}

// ------------------------------------------------------------
// 回环模式的三条约束（★ 与"回环开没开"无关，任何时候都得成立）
// ------------------------------------------------------------
// 为什么要**无条件**测它们，而不是"打开回环时才测"：回环是**编译期**开关、
// 平时关着，于是"关着的时候没人看着这三个常量"—— 那正是最容易改坏又没人发现的地方
// （改坏了要等到某天真去回环才发现，而那时你会先怀疑线、怀疑固件、怀疑板子）。
static void test_link_loopback_wiring_is_sane_even_when_disabled(void) {
  // ① 两个脚是两根**不同**的脚（本机回环 = 一根线把 TX 短接到 RX）
  TEST_ASSERT_NOT_EQUAL((int)link::kLoopbackTxPinC, (int)link::kLoopbackRxPinC);

  // ② ★ 都不许是 43/44：裸 S3 devkit 上那两脚接着**板载 USB-串口桥**
  //    （CH340/CH343P）。短接它们等于把桥的推挽 TX 一起并进回路（对打），
  //    而 §8 L1 说的那颗 FSUSB42UMX 是"二选一"的模拟开关 —— **摘不掉**桥。
  TEST_ASSERT_TRUE_MESSAGE(link::kLoopbackTxPinC != 43 && link::kLoopbackTxPinC != 44,
                           "回环的 TX 脚不许用 GPIO43/44：会与板载串口桥的推挽 TX 并线");
  TEST_ASSERT_TRUE_MESSAGE(link::kLoopbackRxPinC != 43 && link::kLoopbackRxPinC != 44,
                           "回环的 RX 脚不许用 GPIO43/44：会与板载串口桥的推挽 TX 并线");

  // ③ 回环的 UART 与链路的 UART **不是同一个**：
  //    UART0 的 43/44 上挂着桥 ⇒ UART0 **在硬件上**做不了本机回环，这不是软件选择。
  TEST_ASSERT_TRUE_MESSAGE(link::kLoopbackUsesOwnUart,
                           "回环不能与链路用同一个 UART：UART0 的 43/44 上挂着板载桥，摘不掉");
  TEST_ASSERT_EQUAL_INT(1, (int)link::kLoopbackUartPort);   // 1 = UART1
}

// 回环默认**关**（用户口径：默认关闭）。这条看着像废话，但它挡的是"某次调试
// 顺手把默认值改成 1 忘了改回来"——那会让上板后的链路端口/引脚**静默**变成 17/18。
static void test_link_loopback_is_off_by_default(void) {
  TEST_ASSERT_FALSE(link::kLoopbackEnabled);
}

// 编译期守卫的"运行期影子"：pins 头文件里有一条**无条件**的 static_assert
// 检查这三个条件（见 kLoopbackWiringSane 与 pins_detail::LoopbackWiringMustBeSane）。
// 本用例只是把同一条判据再写成可执行断言 —— 将来若有人把那条 static_assert 删掉，
// 这里还能红。
static void test_link_pins_compile_time_guard_has_runtime_twin(void) {
  TEST_ASSERT_TRUE(link::kLoopbackWiringSane);
}

void register_link_phy_uart_tests(void) {
  RUN_TEST(test_link_pins_match_contract);
  RUN_TEST(test_link_pins_are_role_independent);
  RUN_TEST(test_link_loopback_wiring_is_sane_even_when_disabled);
  RUN_TEST(test_link_loopback_is_off_by_default);
  RUN_TEST(test_link_pins_compile_time_guard_has_runtime_twin);
}

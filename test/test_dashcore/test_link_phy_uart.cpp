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
  TEST_ASSERT_EQUAL_INT(43, (int)dashlink::kLinkTxPin);
  TEST_ASSERT_EQUAL_INT(44, (int)dashlink::kLinkRxPin);
  // §0「载体」：链路用的就是 **UART0 那一对脚（43/44）**
  TEST_ASSERT_EQUAL_INT(0, (int)dashlink::kLinkUartPort);
  // §1.1：**115200 8N1**
  TEST_ASSERT_EQUAL_UINT32(115200u, dashlink::kLinkBaud);
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
  TEST_ASSERT_TRUE(dashlink::kLocalRole == dashlink::kRoleMaster || dashlink::kLocalRole == dashlink::kRoleSlave);
  TEST_ASSERT_NOT_EQUAL((int)dashlink::kLinkTxPin, (int)dashlink::kLinkRxPin);

  // 43/44 是板载 USB-串口桥（CH343P）那一对，VAN 特意避开它们走 GPIO15
  // （«本方案用途»表：GPIO15 = 主板 VAN 收发器 RX）⇒ 链路与 VAN 不许抢同一根脚。
  // ★ 这里只钉"链路这一侧"的 43/44；VAN 那根脚在 van_phy_gpio.cpp，
  //   由那条"特意避开 43/44"的口径与文档一起兜着。
  TEST_ASSERT_TRUE(dashlink::kLinkTxPin == 43 || dashlink::kLinkTxPin == 44);
  TEST_ASSERT_TRUE(dashlink::kLinkRxPin == 43 || dashlink::kLinkRxPin == 44);
}

// ------------------------------------------------------------
// 回环模式的三条约束（★ 与"回环开没开"无关，任何时候都得成立）
// ------------------------------------------------------------
// 为什么要**无条件**测它们，而不是"打开回环时才测"：回环是**编译期**开关、
// 平时关着，于是"关着的时候没人看着这三个常量"—— 那正是最容易改坏又没人发现的地方
// （改坏了要等到某天真去回环才发现，而那时你会先怀疑线、怀疑固件、怀疑板子）。
static void test_link_loopback_wiring_is_sane_even_when_disabled(void) {
  // ① 两个脚是两根**不同**的脚（本机回环 = 一根线把 TX 短接到 RX）
  TEST_ASSERT_NOT_EQUAL((int)dashlink::kLoopbackTxPinC, (int)dashlink::kLoopbackRxPinC);

  // ② ★ 都不许是 43/44：裸 S3 devkit 上那两脚接着**板载 USB-串口桥**
  //    （CH340/CH343P）。短接它们等于把桥的推挽 TX 一起并进回路（对打），
  //    而 §8 L1 说的那颗 FSUSB42UMX 是"二选一"的模拟开关 —— **摘不掉**桥。
  TEST_ASSERT_TRUE_MESSAGE(dashlink::kLoopbackTxPinC != 43 && dashlink::kLoopbackTxPinC != 44,
                           "回环的 TX 脚不许用 GPIO43/44：会与板载串口桥的推挽 TX 并线");
  TEST_ASSERT_TRUE_MESSAGE(dashlink::kLoopbackRxPinC != 43 && dashlink::kLoopbackRxPinC != 44,
                           "回环的 RX 脚不许用 GPIO43/44：会与板载串口桥的推挽 TX 并线");

  // ③ 回环的 UART 与链路的 UART **不是同一个**：
  //    UART0 的 43/44 上挂着桥 ⇒ UART0 **在硬件上**做不了本机回环，这不是软件选择。
  TEST_ASSERT_TRUE_MESSAGE(dashlink::kLoopbackUsesOwnUart,
                           "回环不能与链路用同一个 UART：UART0 的 43/44 上挂着板载桥，摘不掉");
  TEST_ASSERT_EQUAL_INT(1, (int)dashlink::kLoopbackUartPort);   // 1 = UART1
}

// 回环默认**关**（用户口径：默认关闭）。这条看着像废话，但它挡的是"某次调试
// 顺手把默认值改成 1 忘了改回来"——那会让上板后的链路端口/引脚**静默**变成 17/18。
static void test_link_loopback_is_off_by_default(void) {
  TEST_ASSERT_FALSE(dashlink::kLoopbackEnabled);
}

// 编译期守卫的"运行期影子"：pins 头文件里有一条**无条件**的 static_assert
// 检查这三个条件（见 kLoopbackWiringSane 与 pins_detail::LoopbackWiringMustBeSane）。
// 本用例只是把同一条判据再写成可执行断言 —— 将来若有人把那条 static_assert 删掉，
// 这里还能红。
static void test_link_pins_compile_time_guard_has_runtime_twin(void) {
  TEST_ASSERT_TRUE(dashlink::kLoopbackWiringSane);
}

// ------------------------------------------------------------
// ★★ 两个角色**都带真 PHY**之后的那条口径（2026-09-25 新增）
// ------------------------------------------------------------
// 背景：在"只有主板那一侧编真 UART"的那一版里，从板（LINK_ROLE=0）的 PHY 是空壳
// （`LinkPhyNull`）⇒ 上面第一条用例测的那组常量**只对主板有意义**。2026-09-25 起
// 两块板都接真 PHY（v1 要的是双向：B→A 的 STATUS/EVENT，见 §0 那条"本节对
// 「接线定案」的一处新增"），于是"同一组脚、对两个角色都成立"这件事必须**被钉住**：
//   · 它要是哪天被改成"按角色分叉"（主板 43/44、从板 44/43），两块板的接线表就会
//     分家 —— 而 4 线线缆里只有**一对** TX/RX交叉，必有一端接反；
//   · 这种错在编译期**没有任何信号**（脚号是宏，怎么写都编得过），在现场的表现
//     只是"发出去没人收"，与"对端没上电"长得一模一样。
static void test_link_phy_is_wired_for_both_roles(void) {
  // ① 两个角色取的是**同一份** `link_phy_pins.h` 常量：谁都是"43 发、44 收"。
  //    本翻译单元与 `link_role.h` 一起编译 ⇒ `LINK_ROLE` 取 0 或 1，
  //    下面四个断言都得成立（它们**不**读 kLocalRole 去分支）。
  TEST_ASSERT_EQUAL_INT(43, (int)dashlink::kLinkTxPin);
  TEST_ASSERT_EQUAL_INT(44, (int)dashlink::kLinkRxPin);
  TEST_ASSERT_EQUAL_INT(0, (int)dashlink::kLinkUartPort);
  TEST_ASSERT_EQUAL_UINT32(115200u, dashlink::kLinkBaud);

  // ② 真 PHY 用的**就是**这两个脚，而且与"日志 UART"那一对是同一对外设 ——
  //    所以"日志不许也占 UART0"那条闸门（link_phy_uart.cpp 的 #error）判的
  //    正是这份固件的生死线。这里把它的**前提**钉住：链路端口 = 0。
  TEST_ASSERT_EQUAL_INT(0, (int)dashlink::kLinkUartPort);

  // ③ 回环那一套脚**仍然**是另一组（17/18 + UART1），没有被"两个角色都带 PHY"
  //    这件事牵连：回环还是回环，链路还是 43/44。两边混起来的话，
  //    `env:esp32s3-linkloop` 会在 43/44 上做回环 —— 那正是编译期守卫拦的东西。
  TEST_ASSERT_EQUAL_INT(1, (int)dashlink::kLoopbackUartPort);
  TEST_ASSERT_NOT_EQUAL((int)dashlink::kLoopbackTxPinC, (int)dashlink::kLinkTxPin);
  TEST_ASSERT_NOT_EQUAL((int)dashlink::kLoopbackRxPinC, (int)dashlink::kLinkRxPin);
}

// 从板镜像 == **默认角色**那一档：`link_role.h` 的 `LINK_ROLE` 默认值就是 0，
// 所以 `[env:esp32s3-rgb-slave]` **故意不写** `-DLINK_ROLE=0`（见 platformio.ini
// 那两个 env 的注释）。这条用例钉的是那个选择的**前提**：默认值确实是 0。
//   ★ 哪天有人把默认值改成 1（比如想让"忘记 -D 的构建"变成主板），这条会当场红 ——
//     而那正是"从板镜像会在下一次构建里静默变成主板镜像"这件事的唯一预警。
static void test_link_default_role_is_slave(void) {
  // native 构建不带 -DLINK_ROLE ⇒ 这里读到的就是"默认值"。
  TEST_ASSERT_EQUAL_INT(0, (int)dashlink::kLocalRole);
  TEST_ASSERT_EQUAL_UINT8(dashlink::kRoleSlave, dashlink::kLocalRole);
  TEST_ASSERT_EQUAL_UINT8(dashlink::kRoleMaster, dashlink::peerRoleOf(dashlink::kLocalRole));
}

void register_link_phy_uart_tests(void) {
  RUN_TEST(test_link_pins_match_contract);
  RUN_TEST(test_link_pins_are_role_independent);
  RUN_TEST(test_link_loopback_wiring_is_sane_even_when_disabled);
  RUN_TEST(test_link_loopback_is_off_by_default);
  RUN_TEST(test_link_pins_compile_time_guard_has_runtime_twin);
  RUN_TEST(test_link_phy_is_wired_for_both_roles);
  RUN_TEST(test_link_default_role_is_slave);
}

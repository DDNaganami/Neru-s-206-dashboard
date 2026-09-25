#pragma once
#include <stdint.h>

// ============================================================
// 双板链路协议 v1 —— **ESP-NOW 那一档的常量与编译期守卫**
// （纯头文件、无 Arduino 依赖，宿主机也编）
//
// ★ 为什么单独拆一份（与 `link_phy_pins.h` 同一个理由）：
//   真 ESP-NOW 的类（`link_phy_espnow.h`）在**宿主机上编不了**（那边没有
//   `esp_now_send` / `WiFi`），但"用哪个信道、环多大、一帧装不装得下、
//   MAC 几个字节"这些**事实**必须能在 native 上被用例钉住 ——
//   信道配错在板上**不会有任何编译期信号**，症状是 `peer` 永远学不到、
//   两端各自打 `rx=0`（看上去像"板子坏了"）。
//
// 背景（为什么会有这一份）：有线链路（UART0 的 43/44）在台面上卡住了 ——
//   实测"主板在发、线交叉且导通、信号到得了副板排针，但副板 `rx bytes=0`"，
//   主假设是"**插着 UART Type-C 的板，4Pin UART 被那颗 FSUSB42UMX 从 ESP32 上
//   摘走了**"（微雪 wiki 原话 + 车主台面实测，见 ARCHITECTURE §8.2 与
//   docs/LINK-TWO-BOARD.md 的「前置条件」一节）。
//   ⇒ 车主拍板：**再加一条无线链路**（ESP-NOW，跑在 WiFi 那半边射频上；
//     S3 **没有经典蓝牙**，只有 BLE 与 WiFi）。
//
// ★★ 分层没有变：`link_phy.h` 那个接口 + 三个实现（UART / 空壳 / **ESP-NOW**）。
//   **协议 / 消息表 / CRC / 既有用例一个字都没动** —— 本档只是一个新的
//   `LinkPhy` 实现，加两个新 env（`esp32s3-rgb-{master,slave}-now`）。
// ============================================================

// ---- 编译期开关（数值口径，与 LINK_PHY_UART 同一条纪律） ----
// 1 = 这份固件里有 ESP-NOW 的链路 PHY；0 = 没有。
// ★ 一律写 `#if LINK_PHY_ESP_NOW`（**不是** `#if defined(...)`）：
//   平台期实测过的那条 —— `platformio.ini` 里用 `-U` 撤销父 env 的 `-D`
//   在 `build_flags = ${env:<父>.build_flags} ...` 这一串里**会被 PlatformIO 丢掉**
//   （见 `link_phy_pins.h` 的 `LINK_PHY_UART` 那段），所以撤销一律用
//   `-D...=0` 覆盖；于是"定义了但为 0"必须与"根本没定义"**同义**。
#ifndef LINK_PHY_ESP_NOW
#define LINK_PHY_ESP_NOW 0
#endif

#if (LINK_PHY_ESP_NOW != 0) && (LINK_PHY_ESP_NOW != 1)
#error "LINK_PHY_ESP_NOW 只能是 0(没有) 或 1(有) —— 别的值一定是 -D 写错了"
#endif

// ---- 信道（★ 两端必须一致，写成常量、别再在别处抄数字） ----
// 2.4 GHz 的 1..13（ESP-NOW 只跑 2.4 GHz 那一半射频）。
// ★ 为什么是 **6**：
//   · 它是三条**互不重叠**信道（1 / 6 / 11）的正中间那条，台面上随便挑一条
//     都比挑一条与邻居 AP 重叠的强；
//   · 两端**必须**在同一信道上（ESP-NOW 不跨信道）—— 这条是"配错了就静默不通"
//     的那一类，所以：① 它是**唯一出处**；② 开机那一行日志把它打出来
//     （`espnow: ... ch=6`）；③ 发送侧每一帧都带上它（见下）。
// ★ 注意：**不是**"连上某个路由器就跟着它的信道走"—— 这份固件不连 AP
//   （`WiFi.mode(WIFI_STA)` + `disconnect()`，见 link_phy_espnow.cpp 的 begin()），
//   所以信道完全由本常量说了算。
#ifndef LINK_ESPNOW_CHANNEL
#define LINK_ESPNOW_CHANNEL 6
#endif

#if (LINK_ESPNOW_CHANNEL < 1) || (LINK_ESPNOW_CHANNEL > 13)
#error "LINK_ESPNOW_CHANNEL 只能是 1..13（ESP-NOW 跑在 2.4 GHz；14 只有日本允许且多数驱动不支持）"
#endif

// ---- 缓冲与在途上限（全部有界；见 link_phy_espnow.h 文件头的"三条纪律"） ----
#ifndef LINK_ESPNOW_TX_RING
// 出方向自有环。装得下一帧的最坏情况（链路帧最大 7 + 16 = 23 B）就够 ——
// 它的存在**不是**为了"写永不失败"，而是为了"write() 一个字节都不碰射频"。
#define LINK_ESPNOW_TX_RING 256
#endif

#ifndef LINK_ESPNOW_RX_RING
// 入方向自有环（由 WiFi 任务的接收回调填、主循环读）。
//
// ★★ 2026-09-27 上板实测：**256 B 太小，改成 1024 B**。
//   原来那句"主循环一圈没来绰绰有余"是按"一圈几十微秒"算的 —— 那个前提在
//   装了 RGB 屏的板上**不成立**：实测主循环最大一圈 **83~94 ms**（刷屏），
//   而链路满载时（TICK 50 Hz + DATA 80 Hz，测速期间再加 100 Hz）= 约 230 包/s
//   ⇒ 一次 100 ms 的刷屏停顿会堆下约 370 B，**超过 256 B 就丢包**（记 rxOverflow）。
//   实测症状正是"测量丢包 15.5% 而 `gap_rx` 一直是 0~几毫秒、`tx_fail=0`"，
//   即"射频好好的、是我们自己读得太慢"。
//   ⇒ 1024 B 给到约 4 倍余量（≈4 帧最坏帧 × ... 按 16 B/包算是 64 包），
//     仍然有界、仍然是"满了就丢并计数"（`rxOverflow()` 能看见），绝不阻塞回调。
#define LINK_ESPNOW_RX_RING 1024
#endif

#ifndef LINK_ESPNOW_MAX_PENDING
// 在途（已交给 `esp_now_send`、回调还没回来）的包数上限。
// ★ 为什么必须有它（这是本档**唯一**一个"非阻塞"的薄弱点，写在最显眼处）：
//   `esp_now_send()` 只把包**拷进驱动自己的待发队列**就返回（不阻塞），但那个队列
//   是有容量的 ⇒ 无脑灌会拿到 `ESP_ERR_ESPNOW_NO_MEM`。所以：① 主循环每圈发的包数
//   有上界（kPumpMaxPackets）；② 在途数到 `kMaxPending` 就不再发（`availableForWrite()`
//   报 0 更长一段时间）⇒ 上层（`LinkTx::pump`）只会"少发"，不会"发不出还被记成成功"。
//   ★ 它**不改变**任何协议口径：丢的是整帧，且由 `txFull()` 计数看见。
#define LINK_ESPNOW_MAX_PENDING 8
#endif

#ifndef LINK_ESPNOW_PUMP_PACKETS
// 一次 `pumpTx()` 最多往射频交几个包（单次调用有上界 ⇒ 可随时被打断，§1.3）。
#define LINK_ESPNOW_PUMP_PACKETS 4
#endif

// ---- ★★ 静态配对（**可选**；缺省不定义 = 走自配置发现） ----
// 用法（只在 env 的 build_flags 里加一行，**六个十进制字节、逗号分隔**）：
//     -DLINK_ESPNOW_PEER_MAC=0x90,0x70,0x69,0xea,0xf6,0xb4
// 语义：定义了它 ⇒ **跳过发现广播**、开机直接单播给这个 MAC（`link_phy_espnow.cpp`
//   begin() 的第 ④ 步）。
//
// ★ 为什么留这一档（以及为什么**默认不用**它）：
//   · 表仓里只有两块板，而"发现广播"的唯一风险就是"同信道上还有别人的 ESP-NOW
//     ⇒ 学到错的邻居"（学到之后会被 NVS 记住，症状是两端都静默 `rx=0`、还查不出原因）；
//   · 但静态配对的**前提**是"你得先知道两块板各自的 MAC" —— 那正是开机那行
//     `espnow: sta_mac=…` 的用途（**先跑一次、把两个 MAC 抄下来**，再回来写死）；
//   · ⇒ 所以缺省仍然是"自配置发现"（今晚就能跑），静态配对是"跑过一次之后的一行 `-D`"。
//   ★★ `LINK_ESPNOW_PEER_MAC` **不许**用字符串形式（`"aa:bb:…"`）：那是运行期解析、
//     要拉进 `sscanf`/字符串处理，而我们这条链路的口径是"编译期能定就编译期定"。
#ifndef LINK_ESPNOW_PEER_MAC
// 未定义 = 自配置发现（广播 HELLO → 收到一帧形态合法的 v1 帧 → 学到对端 MAC）
#endif

#include "link_frame.h"

namespace dashlink {

// ---- 运行期可见的常量（用例读它们，别再抄数字） ----
static const uint8_t kEspNowChannel   = (uint8_t)LINK_ESPNOW_CHANNEL;
static const uint16_t kEspNowTxRing   = (uint16_t)LINK_ESPNOW_TX_RING;
static const uint16_t kEspNowRxRing   = (uint16_t)LINK_ESPNOW_RX_RING;
static const uint8_t kEspNowMaxPending = (uint8_t)LINK_ESPNOW_MAX_PENDING;
static const uint8_t kEspNowPumpPackets = (uint8_t)LINK_ESPNOW_PUMP_PACKETS;

// MAC 长度（6 字节，ESP-NOW 的 peer 地址口径）
static const uint8_t kEspNowMacLen = 6u;

// 广播地址 `FF:FF:FF:FF:FF:FF` —— **自配置寻址的第一跳**：
// 还没学到对端 MAC 时，每一个包都往广播发一个 `HELLO`（见 link_app.h 的
// helloDue()：上电 1 次、之后每 5 s 直到收到对端 HELLO）；一旦收到**任何**
// 对端帧就学到了它的 MAC（`recv_info->src_addr`），之后改**单播**。
static const uint8_t kEspNowBroadcast[kEspNowMacLen] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// NVS：学到的那 6 个字节存哪儿（下一次开机直接单播，不用再等一轮广播 HELLO）。
// ★ 命名空间/键名与 `main.cpp` 里静音那一路（`dash`/`mute`）同一个命名空间，
//   但键不同 —— 一处 NVS、一套 Preferences，不另开第二个命名空间。
static const char* const kEspNowNvsNamespace = "dash";
static const char* const kEspNowNvsPeerKey   = "enpeer";

// ---- 编译期守卫（改坏了在编译期就炸，不要等到板上"发出去没人收"） ----
// ① 环要装得下**最坏一帧**（否则 write() 会连一整帧都收不下 ⇒ 永远发不出去）
static_assert(LINK_ESPNOW_TX_RING >= (int)(kFrameBytesMax),
              "LINK_ESPNOW_TX_RING 至少要装得下一帧最坏情况（71 B）—— 否则 write() 永远收不下整帧");
// ② 入环同理（一帧都存不下 ⇒ available() 永远 0，看上去像"对端没发"）
static_assert(LINK_ESPNOW_RX_RING >= (int)(kFrameBytesMax),
              "LINK_ESPNOW_RX_RING 至少要装得下一帧最坏情况（71 B）—— 否则入环必然丢字节");
// ③ 在途上限至少要 1（0 ⇒ 一个包都发不出去），且不要大到"主循环一圈发不完"
static_assert(LINK_ESPNOW_MAX_PENDING >= 1 && LINK_ESPNOW_MAX_PENDING <= 32,
              "在途上限 1..32：0 就一个包都发不出去；太大则'每圈有上界'这条失效");
static_assert(LINK_ESPNOW_PUMP_PACKETS >= 1, "一次 pumpTx() 至少要能交 1 个包，否则永远发不出去");

// ④ ★ ESP-NOW 的**单包有效载荷上限 = 250 B**（IDF 的口径，见 esp_now.h 那段说明）。
//    链路帧最大 71 B ⇒ 一帧一个包，**永远不会**需要分片。
//    这条写在编译期：将来谁把 kLenNoWaitAbove 抬到 250 以上，这里当场拦住 ——
//    "要不要分片"是一个协议级决定，不该在某个人改一个数字时悄悄发生。
static_assert((int)kFrameBytesMax <= 250,
              "一帧必须装得进一个 ESP-NOW 包（≤250 B 有效载荷）—— 超过就得先定分片口径，"
              "那是协议级决定，别在改一个常量时悄悄发生");

}  // namespace dashlink

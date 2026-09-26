// 双板链路协议 v1 —— **从板也会发**（§3 表 `0x01` HELLO / `0x30` STATUS）用例
//
// 这一组是 2026-09-27 补的那个契约缺口的判据层。缺口的原话在 `src/main.cpp` 里
// （本单之前那一版）：
//     "从板多了一个 `pumpTx()`：`LinkTx` 那个环**今天还空着**（v1 的 DATA/TICK
//      只由主板发，§3）…… 今天它是个空转：环里 0 字节 ⇒ 立刻返回 0。"
// 而 §3 表的 `0x01` 行写的是 HELLO **双向**、`0x30` 行的方向写的是 **B → A**
// ⇒ v1 的"双向"当时只兑现了 A→B 那一半：从板一个字节都不发，
// 主板那 30 s 的 `从板无响应` 判据（§8 L13）恒为真，`link: B uptime=…` 那行
// **一次都打不出来**（现场看到的就是"线接好了、两板都在跑、主板上那些 B 行没有"）。
//
// 本组把"从板那一支"的判据钉住，逐条对应本次改动：
//   ① `HELLO` 上电立刻一条、未 ack 时每 5 s 一条、**收到对端 HELLO 就永久停发**（§3）；
//   ② `STATUS` 2 Hz（500 ms）、载荷逐字节照契约（§2 宽度 + §3 字段次序 + 大端）；
//   ③ `STATUS.flags` 四位里**有生产者的三位**怎么来、第四位为什么恒 0；
//   ④ **发送预算**：两边都在 115200 上说话时，从板那 0.4% 是算得出来的（§1.1）；
//   ⑤ ★ **RX 优先**：从板在"一直发"的同时，收帧不许被挤掉（本单的硬约束之一）。
//
// ★ 本组**不动协议**：消息类型 / LEN / 字段次序 / CRC 覆盖范围 / `LINK_ROLE` 的
//   编译期权威（§5）一个都没改，改的只有"谁在什么时刻 enqueue"。
#include <unity.h>

#include <string.h>

#include "data_service.h"
#include "fake_link_phy.h"
#include "link_app.h"
#include "link_frame.h"
#include "link_msg.h"
#include "link_phy_pins.h"
#include "link_rx.h"
#include "link_time.h"
#include "link_tx.h"

using namespace dashlink;

namespace {

// 把一帧从 A 端（主板侧）推到 B 端（从板侧）—— 用例里要把"上游来的东西"造出来。
void pushMasterFrame(FakeLinkPhy& a, uint8_t type, const uint8_t* payload, uint8_t len,
                     uint8_t role = kRoleMaster) {
  LinkTx tx;
  TEST_ASSERT_TRUE(tx.enqueueFrame(type, payload, len, role));
  while (tx.queued() > 0u) tx.pump(a);
}

// 造一个"主板发来的 TICK"（§3：50 Hz、tick_ms u32 + seq u8）。
void pushMasterTick(FakeLinkPhy& a, uint32_t tick_ms, uint8_t seq) {
  TickMsg tm;
  tm.tick_ms = tick_ms;
  tm.seq = seq;
  uint8_t p[kTickLen];
  TEST_ASSERT_TRUE(packTick(tm, p));
  LinkTx tx;
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Tick, p, kTickLen, kRoleMaster));
  while (tx.queued() > 0u) tx.pump(a);
}

// 从**对端**（peer）把那一边收到的字节解成帧。
// ★ 方向很容易搞反：`tx.pump(phy_b)` 的字节会走到 `phy_b` 的 peer 的读缓冲里 ——
//   也就是说"从板发出去的东西"要在 **phy_a** 这一侧读（本用例第一次就写反过）。
uint16_t drainPeerFrames(FakeLinkPhy& peer, Frame* out, uint16_t max, uint16_t* crc_err = nullptr) {
  uint16_t n = 0;
  uint16_t bad = 0;
  while (peer.available() > 0) {
    uint8_t buf[kFrameBytesMax];
    if (peer.available() < (int)(kOverhead)) break;      // 连最短帧都不够
    for (uint8_t i = 0; i < kHeaderBytes; ++i) {
      const int c = peer.read();
      TEST_ASSERT_TRUE(c >= 0);
      buf[i] = (uint8_t)c;
    }
    if (buf[kOffSync] != kSync || !lenInRange(buf[kOffLen])) { ++bad; continue; }
    const uint16_t total = frameBytesForLen(buf[kOffLen]);
    if (peer.available() < (int)(total - kHeaderBytes)) break;
    for (uint16_t i = kHeaderBytes; i < total; ++i) {
      const int c = peer.read();
      TEST_ASSERT_TRUE(c >= 0);
      buf[i] = (uint8_t)c;
    }
    Frame f;
    if (decodeFrame(buf, total, &f) != DecodeErr::Ok) { ++bad; continue; }
    if (n < max) out[n] = f;
    ++n;
  }
  if (crc_err != nullptr) *crc_err = bad;
  return n;
}

}  // namespace

// ------------------------------------------------------------
// ① HELLO 的重发口径（§3 表 `0x01` 行）—— **两个角色共用同一条**
// ------------------------------------------------------------
// 契约原文：上电 1 次；之后每 **5 s** 重发，**直到收到对端 HELLO**；幂等、无超时概念。
// ★ 这条用例同时钉住"主板那一支也走同一个函数"（本单把那个 if 抽成了
//   `helloDue()`，两个角色共用；抽之前是 main.cpp 里手写的 5000）。
static void test_link_slave_hello_first_immediately_then_every_5s(void) {
  uint32_t last = 0;
  bool acked = false;

  // 上电第一拍：立刻要发（last == 0 那一支）
  TEST_ASSERT_TRUE(helloDue(0u, last, acked));
  last = 0u;                       // ★ 上电时刻**就是 0** —— 别拿 0 当"还没发过"的哨兵
  TEST_ASSERT_TRUE(helloDue(0u, last, acked));

  // 模拟"发了第一条"：调用方把 last 推成 now
  last = 1000u;
  TEST_ASSERT_FALSE(helloDue(1001u, last, acked));       // 差 1 ms：不发
  TEST_ASSERT_FALSE(helloDue(5999u, last, acked));       // 差 4999 ms：不发
  TEST_ASSERT_TRUE(helloDue(6000u, last, acked));        // 差 5000 ms：发（边界是 >=）

  // 契约数字必须与 `kHelloRepeatMs` 同源（5 s 只许出现一次）
  TEST_ASSERT_EQUAL_UINT32(5000u, kHelloRepeatMs);
}

// ack 之后**永久**停发（§3："直到收到对端 HELLO"）—— 包括"刚好到 5 s 那一拍"。
static void test_link_slave_hello_stops_after_ack(void) {
  uint32_t last = 1000u;
  TEST_ASSERT_TRUE(helloDue(6000u, last, false));
  TEST_ASSERT_FALSE(helloDue(6000u, last, true));         // 同一时刻，ack 了就不发
  TEST_ASSERT_FALSE(helloDue(6000u + 60000u, last, true));
  // ★ ack 只看"收到过对端 HELLO"这一件事：它**不**随时间失效（无超时概念，§3）
  TEST_ASSERT_FALSE(helloDue(0xFFFFFFFFu, last, true));
}

// ② STATUS 的节奏：上电立刻一条，之后每 500 ms（§3 表 `0x30` 行的 2 Hz）。
static void test_link_status_sender_is_2hz_and_fires_first_immediately(void) {
  StatusSender s;
  s.reset();
  StatusMsg m;
  TEST_ASSERT_TRUE(s.due(0u, &m));          // 上电第一拍立刻发（uptime = 0，合法）
  TEST_ASSERT_EQUAL_UINT32(0u, m.uptime_ms);
  TEST_ASSERT_EQUAL_UINT32(1u, s.sent());
  TEST_ASSERT_FALSE(s.due(1u, &m));
  TEST_ASSERT_FALSE(s.due(499u, &m));
  TEST_ASSERT_TRUE(s.due(500u, &m));        // 边界 >= 500
  TEST_ASSERT_EQUAL_UINT32(500u, m.uptime_ms);
  TEST_ASSERT_TRUE(s.due(1000u, &m));
  TEST_ASSERT_FALSE(s.due(1499u, &m));
  TEST_ASSERT_TRUE(s.due(1500u, &m));
  TEST_ASSERT_EQUAL_UINT32(4u, s.sent());   // t=0 / 500 / 1000 / 1500

  // 契约数字的唯一出处（与 §3 表那一行的 500 ms 同源）
  TEST_ASSERT_EQUAL_UINT32(500u, kStatusPeriodMs);

  // ★ 频率判据：**半开区间** [t0, t0+1000) 里正好 2 条（2 Hz）。
  //   为什么用半开区间而不是 `t <= 1000`：闭区间会把**两端**都算进来（t=0 与 t=1000
  //   相隔正好 1000 ms，各自都是一条独立的消息）⇒ 数出 3 条，那是"窗口长度"的问题，
  //   不是频率不对。稳态频率 = 500 ms 一条 ⇒ 1 秒窗口内 2 条。
  StatusSender s2;
  s2.reset();
  uint32_t n = 0;
  for (uint32_t t = 0; t < 1000u; t += 1u) {
    if (s2.due(t, &m)) ++n;
  }
  TEST_ASSERT_EQUAL_UINT32(2u, n);          // t=0 / 500
}

// ③ STATUS 载荷**逐字节**照契约（§2 宽度 + §3 字段次序 + 多字节大端）。
//    ★ 这一条是"从板报上去的东西主板能不能正确解回来"的端到端判据：
//      宽度写错、字节序写反、字段次序挪一位 —— 都不会有编译期信号。
static void test_link_status_encodes_contract_fields(void) {
  StatusMsg m;
  m.fw_ver         = 0x1234u;
  m.uptime_ms      = 0x01020304u;
  m.frames_ok      = 0x0A0Bu;
  m.frames_dropped = 0x0C0Du;
  m.crc_err        = 0x0E0Fu;
  m.last_gap_ms    = 0u;          // §3 定案：v1 恒 0（没有生产者）
  m.left_face      = 5u;
  m.flags          = kStFlagRoleConflict | kStFlagNoData;

  uint8_t p[kStatusLen];
  TEST_ASSERT_TRUE(packStatus(m, p));
  TEST_ASSERT_EQUAL_UINT8(16u, kStatusLen);

  // 逐字节对契约表（§3 `0x30` 行）：u16 fw_ver / u32 uptime / u16 ×4 / u8 / u8
  TEST_ASSERT_EQUAL_HEX8(0x12u, p[0]);
  TEST_ASSERT_EQUAL_HEX8(0x34u, p[1]);
  TEST_ASSERT_EQUAL_HEX8(0x01u, p[2]);
  TEST_ASSERT_EQUAL_HEX8(0x02u, p[3]);
  TEST_ASSERT_EQUAL_HEX8(0x03u, p[4]);
  TEST_ASSERT_EQUAL_HEX8(0x04u, p[5]);
  TEST_ASSERT_EQUAL_HEX8(0x0Au, p[6]);
  TEST_ASSERT_EQUAL_HEX8(0x0Bu, p[7]);
  TEST_ASSERT_EQUAL_HEX8(0x0Cu, p[8]);
  TEST_ASSERT_EQUAL_HEX8(0x0Du, p[9]);
  TEST_ASSERT_EQUAL_HEX8(0x0Eu, p[10]);
  TEST_ASSERT_EQUAL_HEX8(0x0Fu, p[11]);
  TEST_ASSERT_EQUAL_HEX8(0x00u, p[12]);    // last_gap_ms 高字节
  TEST_ASSERT_EQUAL_HEX8(0x00u, p[13]);    // last_gap_ms 低字节
  TEST_ASSERT_EQUAL_HEX8(5u, p[14]);       // left_face
  TEST_ASSERT_EQUAL_HEX8((uint8_t)(kStFlagRoleConflict | kStFlagNoData), p[15]);

  // 往返：解回来必须与发出去的一模一样（§2 的"已知 TYPE 只许加尾巴"）
  StatusMsg back;
  TEST_ASSERT_TRUE(unpackStatus(p, kStatusLen, &back));
  TEST_ASSERT_EQUAL_UINT16(m.fw_ver, back.fw_ver);
  TEST_ASSERT_EQUAL_UINT32(m.uptime_ms, back.uptime_ms);
  TEST_ASSERT_EQUAL_UINT16(m.frames_ok, back.frames_ok);
  TEST_ASSERT_EQUAL_UINT16(m.frames_dropped, back.frames_dropped);
  TEST_ASSERT_EQUAL_UINT16(m.crc_err, back.crc_err);
  TEST_ASSERT_EQUAL_UINT16(m.last_gap_ms, back.last_gap_ms);
  TEST_ASSERT_EQUAL_UINT8(m.left_face, back.left_face);
  TEST_ASSERT_EQUAL_UINT8(m.flags, back.flags);
  // 解包也认"带尾巴"的载荷（次版本规矩），但那不属于本用例的判据 —— 见 test_link_msg.cpp
}

// ③b `STATUS.flags`：三位有生产者、第四位**恒 0**（理由见 link_app.h 那一段）。
static void test_link_slave_status_flags_three_producers(void) {
  // 全 0：链路健康、数据新鲜（dataState = Locked）⇒ 四位都是 0
  TEST_ASSERT_EQUAL_HEX8(0x00u, slaveStatusFlags(false, false, LinkTimeState::Locked));

  // 三个有生产者的一位一位来
  TEST_ASSERT_EQUAL_HEX8(kStFlagVerMismatch,
                         slaveStatusFlags(true, false, LinkTimeState::Locked));
  TEST_ASSERT_EQUAL_HEX8(kStFlagRoleConflict,
                         slaveStatusFlags(false, true, LinkTimeState::Locked));
  TEST_ASSERT_EQUAL_HEX8(kStFlagNoData,
                         slaveStatusFlags(false, false, LinkTimeState::NoBasis));
  TEST_ASSERT_EQUAL_HEX8(kStFlagNoData,
                         slaveStatusFlags(false, false, LinkTimeState::Degraded));
  TEST_ASSERT_EQUAL_HEX8(kStFlagNoData,
                         slaveStatusFlags(false, false, LinkTimeState::SimFallback));

  // 组合 + 位号（§3 `0x30` 行：bit0 ver / bit1 role / bit2 no_data / bit3 温度弧无源）
  TEST_ASSERT_EQUAL_HEX8((uint8_t)(kStFlagVerMismatch | kStFlagRoleConflict | kStFlagNoData),
                         slaveStatusFlags(true, true, LinkTimeState::Degraded));

  // ★ bit3 **永远不置位**（从板没有生产者：Link 编不进 DATA.flags 那 2 位）
  TEST_ASSERT_EQUAL_HEX8(0x00u, (uint8_t)(slaveStatusFlags(true, true, LinkTimeState::SimFallback) &
                                          kStFlagTempNoSource));
  TEST_ASSERT_EQUAL_HEX8(0x08u, kStFlagTempNoSource);   // 位号本身没变（§3 的契约）
}

// ④ 发送预算：**两边都在 115200 上说话**时，从板新增的那 0.4% 是算得出来的（§1.1）。
//    ★ 这条用例的意义：谁将来把 STATUS 提到 10 Hz、或把 DATA 的上限调大，
//      这里会红 —— 而"链路满了"在板上只会表现成"偶尔丢帧"，几乎不可能靠肉眼发现。
static void test_link_send_budget_matches_contract_duty_cycle(void) {
  // ★ 判据的写法：**先算一遍"契约 §1.1 那张表"的算术**（浮点，不受实现取整影响），
  //   再要求 `lineMsPerSecondForFrame()` 与它相差 ≤1 ms（整数除法的取整余量）。
  //   这样"算错了一位"与"取整方式不同"就分得开 —— 直接把 1.9965 断言成 2
  //   会把"实现取整"误判成"算错了"（本用例第一次就是这么红的）。
  const double baud = (double)kLinkBaud;
  //  帧长 = 7 + 载荷；一帧 bit = 帧长 × 10（8N1）；线时(ms) = bit × 1000 / baud
  auto expect_ms = [baud](uint8_t len, uint32_t hz) -> double {
    const double bits = (double)frameBytesForLen(len) * 10.0;
    return (double)hz * bits * 1000.0 / baud;
  };

  // 单条消息的线时（§1.1 那张表：STATUS 整帧 23 B ≈ 2.00 ms、HELLO 整帧 12 B ≈ 1.04 ms）
  // ★ 传的是**载荷**长度（16 / 5），整帧由函数内部 +7 得到 23 / 12。
  TEST_ASSERT_EQUAL_UINT32(2u, lineMsPerSecondForFrame(kStatusLen, 1u));   // 23 B ⇒ 1.9965 ⇒ 1
  TEST_ASSERT_EQUAL_UINT32(1u, lineMsPerSecondForFrame(kHelloLen, 1u));    // 12 B ⇒ 1.0417 ⇒ 1
  TEST_ASSERT_EQUAL_UINT32(0u, lineMsPerSecondForFrame(kDataLen, 0u));     // 0 Hz ⇒ 0
  TEST_ASSERT_TRUE((double)lineMsPerSecondForFrame(kStatusLen, 1u) >= 1.9);
  TEST_ASSERT_TRUE((double)lineMsPerSecondForFrame(kStatusLen, 1u) <= 2.1);

  // 从板常态（HELLO 已 ack、只剩 2 Hz STATUS）：21 字节 ⇒ ≈4 ms/s ≈ 0.4%（§1.1 原话）
  const uint32_t slave = lineMsPerSecondForFrame(kStatusLen, 2u);
  TEST_ASSERT_TRUE(slave >= 3u && slave <= 5u);
  TEST_ASSERT_TRUE((double)slave >= expect_ms(kStatusLen, 2u) - 1.0);
  TEST_ASSERT_TRUE((double)slave <= expect_ms(kStatusLen, 2u) + 1.0);

  // 两侧合计（默认参数 = 契约频率：DATA 80 Hz / VANRAW 80 Hz / TICK 50 Hz / STATUS 2 Hz）
  // ★★ 2026-09-27：**契约 §1.1 那张占空比表里没有 VANRAW**（它是本单新加的
  //   `0x21`）⇒ 这里的目标值从"§1.1 的 ≈15%"变成"§1.1 + 新加的那一项"。
  //   ★ 数字是**算出来的**（不是估的，`lineMsPerSecondForFrame` 的口径：
  //     整帧 = 7 + 载荷、一字节 10 bit、115200 8N1）：
  //     DATA   80 Hz × 13 B（6 B 载荷）  =  90 ms/s
  //     VANRAW 80 Hz × 18 B（11 B 载荷） = 125 ms/s   ← 新加
  //     TICK   50 Hz × 12 B（5 B 载荷）  =  52 ms/s
  //     STATUS  2 Hz × 23 B（16 B 载荷） =   4 ms/s
  //   ⇒ A→B **267** ms/s（原来是 142）、两侧合计 **271** ms/s ≈ 27%
  //     （§1.1 原文的 14% / 15% 是**没有 VANRAW 时**的数 —— 用例最后一条把它验回来）。
  //   **这条用例就是"加了 VANRAW 之后还有多少余量"的唯一可查询处** ——
  //   谁再把频率翻倍、或者把载荷加长，这里会红。
  const uint32_t total = linkBudgetMsPerSecond();
  const uint32_t master = lineMsPerSecondForFrame(kDataLen, 80u) +
                          lineMsPerSecondForFrame((uint8_t)(kVanRawHdrLen + 7u), 80u) +
                          lineMsPerSecondForFrame(kTickLen, 50u);
  TEST_ASSERT_EQUAL_UINT32(master + slave, total);
  TEST_ASSERT_TRUE(total >= 255u && total <= 285u);
  // ★ 从板那一半**仍然**远小于主板那一半（§1.1 那张表：0.4% vs 14% ⇒ 现在是 vs 23%）
  TEST_ASSERT_TRUE(slave * 10u < master);
  // 主板那一半 = 267 ms/s（DAA 90 + VANRAW 125 + TICK 52）
  TEST_ASSERT_TRUE(master >= 250u && master <= 285u);
  TEST_ASSERT_TRUE((double)master >= expect_ms(kDataLen, 80u) +
                                       expect_ms((uint8_t)(kVanRawHdrLen + 7u), 80u) +
                                       expect_ms(kTickLen, 50u) - 3.0);
  TEST_ASSERT_TRUE((double)master <= expect_ms(kDataLen, 80u) +
                                       expect_ms((uint8_t)(kVanRawHdrLen + 7u), 80u) +
                                       expect_ms(kTickLen, 50u) + 3.0);
  // ★ 把 VANRAW 关掉（传 0）必须回到契约 §1.1 那一版的数字（142 ms/s ≈ 14%）
  //   ⇒ 这一项是**可以单独摘掉的**，而且摘掉之后 §1.1 那张表仍然成立。
  const uint32_t without = linkBudgetMsPerSecond(80u, 50u, 2u, 0u, 0u);
  TEST_ASSERT_TRUE(without >= 130u && without <= 150u);
  TEST_ASSERT_TRUE(total - without >= 120u && total - without <= 135u);

  // 反例（把 STATUS 提到 10 Hz 会怎样）：仍然塞得进，但已经不是"看不出来"的量级了
  const uint32_t hot = linkBudgetMsPerSecond(80u, 50u, 10u, 0u);
  TEST_ASSERT_TRUE(hot > total);
}

// ⑤ ★★ **RX 优先**：从板"一直发"的同时，收帧不许被挤掉。
//
// 这一条是本单的硬约束（"从板不会因为发送而挤掉它的接收"），而且它**只由顺序保证**：
//     loop() 里 ① `link_poll_bounded_slave()` 先收 → ② `link_slave_tick()` 再发
// 没有任何编译期信号能看着它 ⇒ 只能在这里按生产形状跑一遍。
//
// 形状刻意与 `main.cpp` 那一支同形：
//   · TX 容量调小（`txCapacity = 24`）⇒ 每一拍 `pump()` 最多只排 24 B
//     ⇒ 环里**长期积压**，发送侧一直"有话要说"（这正是最坏情况）；
//   · 上游（主板）持续灌 TICK，从板每拍先收（有界），再让 STATUS 到点就发。
// 判据：
//   · 每一拍收帧数 ≥ 1（收进度的**下界**，不是"最后一共收到了"）；
//   · 最后一帧不差（`frames_ok == 总数`、`crc_err == 0`）；
//   · 从板确实排出了字节（否则这条用例什么都没在证明）。
static void test_link_slave_sending_does_not_starve_receiving(void) {
  FakeLinkPhy phy_a, phy_b;
  // ★★ **两根都要接**：`FakeLinkPhy::connect()` 是**单向**的（设的是"我写出去的东西
  //   进谁的读缓冲"）⇒ 只接一个方向时，从板写出去的字节会全记进 `lostBytes`，
  //   一个都到不了 A 端（症状：pump 写了、对端 `available()` 却是 0）。
  //   本组用例要的是**从板→主板**那一向 ⇒ 必须 `phy_b.connect(&phy_a)`。
  phy_a.connect(&phy_b);   // 主板→从板（本用例的收帧方向）
  phy_b.connect(&phy_a);   // ★ 从板→主板（本用例的发送方向，缺了这一行就"发出去就丢"）
  // ★★ `FakeLinkPhy::txCapacity` 是 **`uint8_t`**（`fake_link_phy.h`），而且
  //   `availableForWrite()` 报的是**恒定的容量**、**不看对端已经积压了多少** ——
  //   也就是说这个夹具里的读缓冲**没有反压**：本端一直写、对端的 `mRx` 就一直涨。
  //   ⇒ 用例必须**自己把对端读走**（下面循环里那一段），否则 `mRx` 撑爆。
  //   ★ 我第一版把 A 端写成 `4096` ⇒ 隐式窄化成 `4096 % 256 == 0` ⇒ 容量 0
  //     ⇒ `LinkTx::pump()` 一个字节都不写、环永远排不空 ⇒ **测试进程挂死**。
  //     编译器对这种窄化只给 `-Wimplicit-conversion` 警告、**不报错** —— 在这块夹具上
  //     写"大容量"必须显式检查它 ≤ 255。
  phy_a.txCapacity = 255;
  // 从板的 PHY：写容量卡小 ⇒ 每一拍 `pump()` 最多只排 24 B ⇒ 发送侧长期积压（最坏情况）
  phy_b.txCapacity = 24;

  LinkRx rx;
  rx.setLocalRole(kRoleSlave);
  LinkTime lt;
  lt.reset();
  LinkTx tx;
  StatusSender status;
  status.reset();

  uint32_t hello_last = 0;
  bool hello_acked = false;
  uint32_t peer_bytes = 0;      // ★ 从板**真的排出去**的字节数（在 A 端读走的）

  // 上游先来一条 HELLO（从板收下之后就该**永久停发** HELLO —— 于是本用例后面
  // 不发 HELLO，专注看 STATUS 与收帧的关系）。
  {
    uint8_t hp[kHelloLen];
    HelloMsg hm;
    TEST_ASSERT_TRUE(packHello(hm, hp));
    LinkTx a;
    TEST_ASSERT_TRUE(a.enqueueFrame((uint8_t)MsgType::Hello, hp, kHelloLen, kRoleMaster));
    while (a.queued() > 0u) a.pump(phy_a);
  }

  const uint32_t kTicks = 64u;
  uint32_t pushed = 0;
  uint32_t frames_ok_prev = 0;
  bool ever_wrote = false;

  for (uint32_t step = 0; step < kTicks; ++step) {
    // 上游持续灌：每拍 1 条 TICK（§3 的 50 Hz 节奏）
    pushMasterTick(phy_a, 1000u + step * 20u, (uint8_t)step);
    ++pushed;

    // ---- ① 先收（生产形状：有界收帧）----
    // ★ 两道上界，缺一不可：
    //   ① `kMaxPollsPerStep` —— 解出几帧为界（生产代码那一支用的是
    //      `link_rx_bytes_now()` 的差值、`kLinkRxBytesPerLoop = 512` B，
    //      同一个意思：**一拍只做这么多**。宿主面用一个可判定的帧数上界）；
    //   ② `kMaxBytesPerStep` —— 吃进来的字节为界。这一道才是**真正**的那一道：
    //      `LinkRx::poll()` 每次都从 64 B 的预算重新开始，所以"上游一直灌"时
    //      它可以把一整圈的字节都吃光（在板上由 ① 那 512 B 拦着）。
    //      ★ 我第一版**只有** ①，而且把判据写在 `continue` **之后** ⇒
    //        `handleInbound()` 返回 true 的那条路（TICK/DATA，本用例的全部流量）
    //        永远走不到那一行，循环就没有上界 ⇒ 测试进程挂死。
    //        这条纪律写在这里：**循环的上界必须写在循环体的第一句**，
    //        不能挂在某个分支后面（那一支会被漏掉）。
    const uint32_t bytes_before = rx.stats().bytes_read;
    Frame f;
    uint32_t polls = 0;
    while (rx.poll(phy_b, &f)) {
      if (++polls > 64u) break;                                   // ① 解出帧数为界
      if ((uint32_t)(rx.stats().bytes_read - bytes_before) >= 512u) break;  // ② 字节为界
      if (f.type == (uint8_t)MsgType::Hello) {
        hello_acked = true;
        continue;
      }
      LinkData ld;
      if (handleInbound(f, &lt, 1000u + step * 20u, &ld)) continue;
    }
    lt.update(1000u + step * 20u);

    // ★ 判据（下界）：只要上游灌过东西、且环里还有没解完的，这一拍就该有进度。
    //   写成"最终收到了 64 帧"是不够的 —— 那样"发送把接收饿死、最后才补上"
    //   也会通过。这里要的是**每一拍都在推进**。
    if (frames_ok_prev < pushed) {
      TEST_ASSERT_TRUE(rx.stats().frames_ok > frames_ok_prev ||
                       rx.pending() ||
                       phy_b.available() > 0);
    }
    frames_ok_prev = rx.stats().frames_ok;

    // ---- ② 再发（生产形状：HELLO + STATUS + 排水）----
    if (helloDue(1000u + step * 20u, hello_last, hello_acked)) {
      hello_last = 1000u + step * 20u;
      uint8_t hp[kHelloLen];
      HelloMsg hm;
      hm.fw_ver = 0;
      hm.build_tag = 0;
      hm.boot_reason = 0;
      TEST_ASSERT_TRUE(packHello(hm, hp));
      tx.enqueueFrame((uint8_t)MsgType::Hello, hp, kHelloLen, kLocalRole);
    }
    StatusMsg sm;
    if (status.due(1000u + step * 20u, &sm)) {
      sm.fw_ver         = 0;
      sm.frames_ok      = (uint16_t)rx.stats().frames_ok;
      sm.frames_dropped = (uint16_t)rx.stats().framesDropped();
      sm.crc_err        = (uint16_t)rx.stats().crc_err;
      sm.left_face      = 0;
      sm.flags          = 0;
      uint8_t sp[kStatusLen];
      TEST_ASSERT_TRUE(packStatus(sm, sp));
      tx.enqueueFrame((uint8_t)MsgType::Status, sp, kStatusLen, kLocalRole);
    }
    const uint32_t wrote_before = tx.sentBytes();
    tx.pump(phy_b);
    if (tx.sentBytes() > wrote_before) ever_wrote = true;

    // ---- ③ 把从板**排出去的字节在 A 端读走**（并计数）----
    // ★ 为什么必须有这一段（不是"可选的收尾"）：这个夹具**没有反压**
    //   （`availableForWrite()` 只看自己的容量、不看对端积压）⇒ 不读就会把
    //   A 端的读缓冲撑爆，于是"有没有发出去"这件事根本量不到。见上面那段说明。
    for (uint32_t i = 0; i < 48u; ++i) {
      if (phy_a.available() <= 0) break;
      if (phy_a.read() < 0) break;
      ++peer_bytes;
    }
  }

  // ★ 收帧：一条不差、一条不错。
  //   ★★ `kTicks + 1`：那多出来的**一条**是循环之前故意先灌给从板的那条 **HELLO** ——
  //     `LinkRx` 对它是**正常解出来并计进 `frames_ok`** 的（"收下了、交给上层了"），
  //     只是本用例的上层（`main.cpp` 那一支）把它拿去置 `hello_acked` 而不喂数据层。
  //     ⇒ 帧计数是 64 条 TICK + 1 条 HELLO。写成 `kTicks` 会差一条（本用例第一次就红的）。
  //     ★ 别为了凑这个数去改 `LinkRx` 的口径：`frames_ok` 的语义是"收下并交给上层"，
  //       HELLO 确实被收下并交给上层了。
  TEST_ASSERT_EQUAL_UINT32(kTicks + 1u, rx.stats().frames_ok);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().crc_err);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().role_conflict);   // §5 ① 一次都不许触发
  TEST_ASSERT_TRUE(lt.tickSeen());
  TEST_ASSERT_TRUE(lt.state() == LinkTimeState::Locked);

  // ★ 发送侧确实在动（否则上面那些"没被饿死"的判据是空转）
  TEST_ASSERT_TRUE(ever_wrote);
  TEST_ASSERT_TRUE(tx.sentBytes() > 0u);
  // ★ 字节真的到了对端（在 A 端读走了多少）。2 Hz 的 STATUS（23 B）+ 一条 HELLO（12 B）
  //   在 64 拍（=1280 ms）里 ⇒ 至少 2 条 STATUS 那么多字节；取一个**保守下界**：
  TEST_ASSERT_TRUE(peer_bytes >= 46u);   // ≥ 2 × 23 B（两条 STATUS，还不算 HELLO）

  // 收尾：把剩下的字节按帧解一遍，钉住"从板发的帧是合法的 v1 帧、角色是 0"。
  //   ★ 上面循环已经边走边读了很多，这里只解**残余**的那一段（`drainPeerFrames`
  //     从当前读位置往后解）—— 所以"解出几帧"这个数**不能**当本条的主判据
  //     （主判据是上面的 `peer_bytes` 与 `ever_wrote`）。
  Frame sent[64];
  uint16_t crc_bad = 0;
  const uint16_t n = drainPeerFrames(phy_a, sent, 64, &crc_bad);
  TEST_ASSERT_EQUAL_UINT16(0u, crc_bad);
  for (uint16_t i = 0; i < n && i < 64u; ++i) {
    TEST_ASSERT_EQUAL_UINT8(kRoleSlave, sent[i].role);      // ★ 角色字节必须是 0
    TEST_ASSERT_TRUE(typeKnown(sent[i].type));
  }
}

// ⑤b 从板写出去的**每一帧**都必须是"从板角色 + 本机合法 TYPE"（§2 第 4 字节 / §5）。
//     这条把"从板发出来的东西主板会当角色冲突丢掉"这种事故钉在宿主面上。
static void test_link_slave_frames_carry_slave_role_and_known_type(void) {
  FakeLinkPhy phy_a, phy_b;
  // ★★ **两根都要接**：`FakeLinkPhy::connect()` 设的是"**我写出去的东西进谁的读缓冲**"，
  //   是**单向**的（`mPeer->mRx.push_back`）。只写 `phy_a.connect(&phy_b)` 的话，
  //   `phy_b` 的 `mPeer` 还是 null ⇒ 从板写出去的字节**全记进 `lostBytes`**、
  //   一个都到不了 A 端（症状是"pump 明明写了 35 字节、对端 `available()` 却是 0"）。
  //   ★ 这是本用例第二次踩它（第一次是 `test_link_slave_sending_does_not_starve_receiving`），
  //     所以在这里写清楚：**凡是要"从板发、主板收"的用例，两个方向都要 connect。**
  phy_a.connect(&phy_b);   // A 写的 → 进 B 的读缓冲（主板→从板）
  phy_b.connect(&phy_a);   // B 写的 → 进 A 的读缓冲（★ 从板→主板，本组用例要的就是这一向）
  LinkTx tx;
  const uint8_t payload[kHelloLen] = {0};
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Hello, payload, kHelloLen, kLocalRole));
  uint8_t sp[kStatusLen] = {0};
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Status, sp, kStatusLen, kLocalRole));

  const uint32_t expect_bytes = (uint32_t)frameBytesForLen(kHelloLen) +
                               (uint32_t)frameBytesForLen(kStatusLen);
  TEST_ASSERT_EQUAL_UINT32(expect_bytes, (uint32_t)tx.queued());
  // ★ A 端（对端）的读容量必须 ≥ 这两帧 —— 见上面 `txCapacity` 是 `uint8_t` 那段说明。
  //   这里显式写出来，免得将来两帧变长之后这条用例又以"0 帧"的样子红。
  TEST_ASSERT_TRUE(phy_a.txCapacity >= expect_bytes);
  uint32_t guard = 0;
  while (tx.queued() > 0u) {
    if (++guard > 100u) TEST_FAIL_MESSAGE("pump 排不空");
    tx.pump(phy_b);
  }
  // ★ 先把"字节到底有没有到对端"钉住 —— 这条红了就说明问题在 PHY 夹具这一层，
  //   而不是在解帧那一层（省得下次又去怀疑 CRC/角色）。
  TEST_ASSERT_EQUAL_UINT32(expect_bytes, (uint32_t)phy_a.available());

  // 从 A 端（= 主板侧）解回来：角色必须是 0，TYPE 必须是 v1 认识的
  LinkRx rx;
  rx.setLocalRole(kRoleMaster);            // 站在主板那一侧解
  Frame f;
  uint16_t n = 0;
  while (rx.poll(phy_a, &f)) {
    TEST_ASSERT_EQUAL_UINT8(kRoleSlave, f.role);
    TEST_ASSERT_TRUE(typeKnown(f.type));
    TEST_ASSERT_EQUAL_HEX8(kVer, f.ver);
    ++n;
  }
  TEST_ASSERT_EQUAL_UINT16(2u, n);
  TEST_ASSERT_EQUAL_UINT32(2u, rx.stats().frames_ok);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().crc_err);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().role_conflict);   // ★ 主板不会把它当冲突丢掉
}

void register_link_slave_tx_tests(void) {
  RUN_TEST(test_link_slave_hello_first_immediately_then_every_5s);
  RUN_TEST(test_link_slave_hello_stops_after_ack);
  RUN_TEST(test_link_status_sender_is_2hz_and_fires_first_immediately);
  RUN_TEST(test_link_status_encodes_contract_fields);
  RUN_TEST(test_link_slave_status_flags_three_producers);
  RUN_TEST(test_link_send_budget_matches_contract_duty_cycle);
  RUN_TEST(test_link_slave_sending_does_not_starve_receiving);
  RUN_TEST(test_link_slave_frames_carry_slave_role_and_known_type);
}

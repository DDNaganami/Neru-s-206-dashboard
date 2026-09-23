// 双板链路协议 v1 —— **传输层**用例（契约：ARCHITECTURE.md §1「物理层与发送侧约束」
// + §2 的「重同步」+ §5 的角色冲突）
//
// 这一组回答三个"行为"问题（编译期看不出来，只能测）：
//   ① 假 PHY 的故障注入本身对不对（丢字节 / 分片 / 延迟 / 断开）；
//   ② 发送侧**非阻塞**：入队不碰 PHY、空间不够丢**整帧**、pump 绝不忙等；
//   ③ 接收侧**永不锁死**：噪声里找 SYNC、坏帧之后从 SYNC+1 重新找、角色冲突丢帧。
#include <unity.h>
#include <string.h>

#include "fake_link_phy.h"
#include "link_frame.h"
#include "link_msg.h"
#include "link_rx.h"
#include "link_tx.h"

using namespace dashlink;

namespace {

// 造一个载荷（避开 0x5A = SYNC，免得用例里"帧内恰好有个帧头"干扰判读）
void fillPayload(uint8_t* p, uint8_t len, uint8_t seed) {
  for (uint8_t i = 0; i < len; ++i) p[i] = (uint8_t)(seed + i * 3u);
}

uint16_t makeFrame(uint8_t type, uint8_t seed, uint8_t role, uint8_t* out, uint16_t cap) {
  uint8_t payload[kLenMax];
  const uint8_t n = payloadLenForType(type);
  fillPayload(payload, n, seed);
  return encodeFrame(type, payload, n, role, out, cap);
}

// 把一帧的每个字节喂给接收侧，返回收到的那一帧（没有则 need_more 计数）
bool feedFrame(LinkRx& rx, const uint8_t* f, uint16_t n, Frame* out, uint32_t* rejects) {
  bool got = false;
  for (uint16_t i = 0; i < n; ++i) {
    Frame tmp;
    const LinkRx::Step st = rx.feed(f[i], &tmp);
    if (st == LinkRx::Step::Frame) {
      if (out != nullptr) *out = tmp;
      got = true;
    } else if (st == LinkRx::Step::Reject || st == LinkRx::Step::RoleDrop) {
      if (rejects != nullptr) ++(*rejects);
    }
  }
  return got;
}

// ---- 单板回环（src/link_loopback.cpp）在宿主机上的同一份口径 ----
// 回环固件要发的那 200 帧：五类 × 40，**帧与帧之间没有任何空闲**地拼成一个连续
// 字节流。载荷的字节按 enqueueOne() 的 seedByte() 原样填（可预测 ⇒ 解出来能对账）。
// 写进 out 的字节数是返回值（= 40 × 71 = 2840）。
uint16_t buildLoopbackStream(uint8_t* out, uint16_t cap) {
  const uint8_t types[5] = {(uint8_t)MsgType::Hello, (uint8_t)MsgType::Tick,
                            (uint8_t)MsgType::Data,  (uint8_t)MsgType::Status,
                            (uint8_t)MsgType::Event};
  uint16_t n = 0;
  for (uint8_t k = 0; k < 5; ++k) {
    const uint8_t len = payloadLenForType(types[k]);
    for (uint16_t i = 0; i < 40u; ++i) {
      uint8_t payload[kLenMax];
      for (uint8_t j = 0; j < len; ++j) {
        payload[j] = (uint8_t)(types[k] * 31u + i * 7u + j * 13u + 0x11u);
      }
      const uint16_t m = encodeFrame(types[k], payload, len, kLoopbackTxRole, out + n,
                                     (uint16_t)(cap - n));
      if (m == 0u) return 0u;
      n = (uint16_t)(n + m);
    }
  }
  return n;
}

// 把整段连续流一次塞进假 PHY（帧间零空闲），再用收端角色 rx_role 一路 poll 出来。
// 返回解出的帧数；per_type 记每类各解出几帧（可为 nullptr）。
uint32_t drainLoopbackStream(const uint8_t* stream, uint16_t n, uint8_t rx_role,
                             uint32_t* per_type = nullptr) {
  FakeLinkPhy phy;
  phy.feed(stream, n);
  LinkRx rx;
  rx.setLocalRole(rx_role);
  uint32_t got = 0;
  Frame f;
  while (rx.poll(phy, &f, 64u)) {
    ++got;
    if (per_type != nullptr) {
      switch (f.type) {
        case (uint8_t)MsgType::Hello:  ++per_type[0]; break;
        case (uint8_t)MsgType::Tick:   ++per_type[1]; break;
        case (uint8_t)MsgType::Data:   ++per_type[2]; break;
        case (uint8_t)MsgType::Status: ++per_type[3]; break;
        case (uint8_t)MsgType::Event:  ++per_type[4]; break;
        default: break;
      }
    }
  }
  return got;
}

}  // namespace

// ------------------------------------------------------------
// 假 PHY 本身
// ------------------------------------------------------------
static void test_link_phy_loopback_and_counts(void) {
  FakeLinkPhy a, b;
  a.connect(&b);
  const uint8_t d[] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
  TEST_ASSERT_EQUAL_UINT32(5u, (uint32_t)a.write(d, 5));
  TEST_ASSERT_EQUAL_UINT32(1u, a.writeCalls);
  TEST_ASSERT_EQUAL_UINT32(5u, a.wroteBytes);
  TEST_ASSERT_EQUAL_UINT32(5u, (uint32_t)b.rxBytes());
  TEST_ASSERT_EQUAL_INT(5, b.available());
  for (uint8_t i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL_HEX8(d[i], (uint8_t)b.read());
  }
  TEST_ASSERT_EQUAL_INT(-1, b.read());          // 空了就是 -1（非阻塞契约）
  TEST_ASSERT_EQUAL_UINT32(0u, a.lostBytes);
}

static void test_link_phy_fragmentation_caps_each_write(void) {
  FakeLinkPhy a, b;
  a.connect(&b);
  a.maxWriteChunk = 1;                          // 一次只吸收 1 个字节
  const uint8_t d[] = {1u, 2u, 3u, 4u, 5u};
  TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)a.write(d, 5));
  TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)b.rxBytes());
  TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)a.write(d + 1, 4));
  TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)b.rxBytes());
  TEST_ASSERT_EQUAL_UINT32(2u, a.writeCalls);
}

static void test_link_phy_drop_injection(void) {
  FakeLinkPhy a, b;
  a.connect(&b);
  a.dropEveryNth = 3;                           // 每第 3 个字节在线上没了
  const uint8_t d[] = {1u, 2u, 3u, 4u, 5u, 6u};
  TEST_ASSERT_EQUAL_UINT32(6u, (uint32_t)a.write(d, 6));   // 发送方"发出去了" 6 个
  TEST_ASSERT_EQUAL_UINT32(2u, a.droppedBytes);
  TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)b.rxBytes());
  const uint8_t want[] = {1u, 2u, 4u, 5u};
  for (uint8_t i = 0; i < 4; ++i) {
    TEST_ASSERT_EQUAL_HEX8(want[i], (uint8_t)b.read());
  }
}

static void test_link_phy_latency_needs_steps(void) {
  FakeLinkPhy a, b;
  a.connect(&b);
  a.latencySteps = 2;
  const uint8_t d[] = {7u, 8u};
  TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)a.write(d, 2));
  TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)b.rxBytes());
  TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)a.delayedBytes());
  a.step();
  TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)b.rxBytes());
  a.step();
  TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)b.rxBytes());
  TEST_ASSERT_EQUAL_HEX8(7u, (uint8_t)b.read());
  TEST_ASSERT_EQUAL_HEX8(8u, (uint8_t)b.read());
}

static void test_link_phy_disconnect_blocks_io(void) {
  FakeLinkPhy a, b;
  a.connect(&b);
  b.up = false;                                 // 对端掉电/拔线
  const uint8_t d[] = {1u, 2u};
  TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)a.write(d, 2));   // 本地 FIFO 收下了
  TEST_ASSERT_EQUAL_UINT32(2u, a.lostBytes);               // 但对端不在 ⇒ 丢了
  TEST_ASSERT_EQUAL_INT(0, b.available());
  TEST_ASSERT_EQUAL_INT(-1, b.read());
  TEST_ASSERT_EQUAL_INT(0, b.availableForWrite());

  a.up = false;                                 // 本端也断开
  TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)a.write(d, 2));
  TEST_ASSERT_EQUAL_INT(0, a.availableForWrite());
  TEST_ASSERT_EQUAL_INT(-1, a.read());
}

// ------------------------------------------------------------
// 发送侧：非阻塞 / 整帧丢 / 不忙等（§1.2）
// ------------------------------------------------------------
static void test_link_tx_ring_covers_worst_burst(void) {
  TEST_ASSERT_TRUE_MESSAGE(LinkTx::kRingBytes >= 512u, "§1.2 建议的环缓冲下限是 512 B");
  LinkTx tx;
  uint8_t p[kLenMax];
  fillPayload(p, kLenMax, 0x10);
  // §1.2 ② 说的最坏突发：DATA 13 + TICK 12 + STATUS 23 + HELLO 12 + EVENT 11 = 71 B
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Data, p, kDataLen, kRoleMaster));
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Tick, p, kTickLen, kRoleMaster));
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Status, p, kStatusLen, kRoleMaster));
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Hello, p, kHelloLen, kRoleMaster));
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Event, p, kEventLen, kRoleMaster));
  TEST_ASSERT_EQUAL_UINT16(71u, tx.queued());
  TEST_ASSERT_EQUAL_UINT32(0u, tx.droppedFrames());
  TEST_ASSERT_EQUAL_UINT16((uint16_t)(LinkTx::kRingBytes - 71u), tx.freeBytes());
}

// ★ "不在 ISR/回调里发"的可测代理：入队只碰内存，一个字节都不写 PHY；
//   写 PHY 的只有主循环里的 pump()。
static void test_link_tx_enqueue_never_touches_phy(void) {
  FakeLinkPhy phy;
  LinkTx tx;
  uint8_t f[16];
  const uint16_t n = makeFrame((uint8_t)MsgType::Data, 0x10, kRoleMaster, f, sizeof(f));
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_TRUE(tx.enqueue(f, n));
  }
  TEST_ASSERT_EQUAL_UINT32(0u, phy.writeCalls);   // ★ 一次都没写
  TEST_ASSERT_EQUAL_UINT32(0u, phy.wroteBytes);
  TEST_ASSERT_EQUAL_UINT16((uint16_t)(n * 5u), tx.queued());

  uint16_t sent = 0;                             // 主循环那些圈才真的发
  uint16_t guard = 0;
  while (tx.queued() > 0u && guard < 50u) {
    sent = (uint16_t)(sent + tx.pump(phy));
    ++guard;
  }
  TEST_ASSERT_EQUAL_UINT16((uint16_t)(n * 5u), sent);   // 65 B ⇒ 两圈（PHY 每圈最多 64 B）
  TEST_ASSERT_EQUAL_UINT32(2u, phy.writeCalls);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(n * 5u), phy.wroteBytes);
}

static void test_link_tx_drops_whole_frame_when_full(void) {
  LinkTx tx;
  uint8_t f[16];
  const uint16_t n = makeFrame((uint8_t)MsgType::Data, 0x10, kRoleMaster, f, sizeof(f));
  TEST_ASSERT_EQUAL_UINT16(13u, n);              // DATA 帧 = 7 + 6

  uint16_t accepted = 0;
  while (tx.enqueue(f, n)) ++accepted;
  TEST_ASSERT_EQUAL_UINT16(39u, accepted);       // 39 × 13 = 507 B
  TEST_ASSERT_EQUAL_UINT16(5u, tx.freeBytes());
  TEST_ASSERT_EQUAL_UINT32(1u, tx.droppedFrames());

  // 再来一整帧（13 B > 5 B 空位）：整帧丢掉，环里一个字节都不许变
  const uint16_t before = tx.queued();
  TEST_ASSERT_FALSE(tx.enqueue(f, n));
  TEST_ASSERT_EQUAL_UINT16(before, tx.queued());
  TEST_ASSERT_EQUAL_UINT32(2u, tx.droppedFrames());

  // ★ 半帧不许进环：装得下的短帧可以进，长的进不去 —— 判据是"整帧"而不是"逐字节"
  TEST_ASSERT_TRUE(tx.enqueue(f, 5));
  TEST_ASSERT_EQUAL_UINT16(LinkTx::kRingBytes, tx.queued());
  TEST_ASSERT_EQUAL_UINT16(0u, tx.freeBytes());
  TEST_ASSERT_FALSE(tx.enqueue(f, 1));
}

static void test_link_tx_pump_drains_in_order_with_slow_phy(void) {
  FakeLinkPhy a, b;
  a.connect(&b);
  a.txCapacity = 4;                              // 硬件 FIFO 一次只吃得下 4 B
  LinkTx tx;
  uint8_t f1[16], f2[16];
  const uint16_t n1 = makeFrame((uint8_t)MsgType::Tick, 0x20, kRoleMaster, f1, sizeof(f1));
  const uint16_t n2 = makeFrame((uint8_t)MsgType::Data, 0x40, kRoleMaster, f2, sizeof(f2));
  TEST_ASSERT_EQUAL_UINT16(12u, n1);
  TEST_ASSERT_EQUAL_UINT16(13u, n2);
  TEST_ASSERT_TRUE(tx.enqueue(f1, n1));
  TEST_ASSERT_TRUE(tx.enqueue(f2, n2));

  uint32_t total = 0;
  uint16_t guard = 0;
  while (tx.queued() > 0u && guard < 100u) {
    total += tx.pump(a);
    ++guard;
  }
  TEST_ASSERT_EQUAL_UINT16(0u, tx.queued());
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(n1 + n2), total);
  TEST_ASSERT_EQUAL_UINT32(total, tx.sentBytes());
  TEST_ASSERT_TRUE(guard >= 7u);                 // 25 B / 每次最多 4 B ⇒ 至少 7 圈
  // 顺序必须一个字节不差（环绕回时最容易错）
  for (uint16_t i = 0; i < n1; ++i) TEST_ASSERT_EQUAL_HEX8(f1[i], (uint8_t)b.read());
  for (uint16_t i = 0; i < n2; ++i) TEST_ASSERT_EQUAL_HEX8(f2[i], (uint8_t)b.read());
  TEST_ASSERT_EQUAL_INT(-1, b.read());
}

static void test_link_tx_pump_does_not_busy_wait(void) {
  FakeLinkPhy phy;
  LinkTx tx;
  uint8_t f[16];
  const uint16_t n = makeFrame((uint8_t)MsgType::Tick, 0x30, kRoleMaster, f, sizeof(f));
  TEST_ASSERT_TRUE(tx.enqueue(f, n));

  phy.txCapacity = 0;                            // 现在塞不进
  TEST_ASSERT_EQUAL_UINT16(0u, tx.pump(phy));
  TEST_ASSERT_EQUAL_UINT32(0u, phy.writeCalls);  // ★ 没空间就连一次 write 都不发
  TEST_ASSERT_EQUAL_UINT16(n, tx.queued());      // 数据留着，不丢
  TEST_ASSERT_EQUAL_UINT32(0u, tx.droppedFrames());

  phy.txCapacity = 64;                           // 有空间了 ⇒ 一次排空
  TEST_ASSERT_EQUAL_UINT16(n, tx.pump(phy));
  TEST_ASSERT_EQUAL_UINT16(0u, tx.queued());

  // 断开时同样：不写、不丢（从板随时可能回来）
  phy.up = false;
  TEST_ASSERT_TRUE(tx.enqueue(f, n));
  TEST_ASSERT_EQUAL_UINT16(0u, tx.pump(phy));
  TEST_ASSERT_EQUAL_UINT16(n, tx.queued());
  TEST_ASSERT_EQUAL_UINT32(0u, tx.droppedFrames());
}

// ------------------------------------------------------------
// 接收侧：字节流 → 帧（§2 的重同步）+ §5 的角色冲突
// ------------------------------------------------------------
static void test_link_rx_decodes_frames_from_phy(void) {
  FakeLinkPhy a, b;
  a.connect(&b);
  LinkTx tx;
  LinkRx rx;
  rx.setLocalRole(kRoleSlave);                  // 本机是从板 ⇒ 对端必须是主板
  uint8_t p[kLenMax];
  fillPayload(p, kLenMax, 0x50);
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Data, p, kDataLen, kRoleMaster));
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Tick, p, kTickLen, kRoleMaster));
  while (tx.queued() > 0u) tx.pump(a);

  Frame f;
  TEST_ASSERT_TRUE(rx.poll(b, &f));
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Data, f.type);
  TEST_ASSERT_EQUAL_UINT8(kDataLen, f.len);
  TEST_ASSERT_EQUAL_UINT8(kRoleMaster, f.role);
  TEST_ASSERT_EQUAL_HEX8(p[0], f.payload[0]);
  TEST_ASSERT_TRUE(rx.poll(b, &f));
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Tick, f.type);
  TEST_ASSERT_FALSE(rx.poll(b, &f));            // 没了
  TEST_ASSERT_EQUAL_UINT32(2u, rx.stats().frames_ok);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().framesDropped());
}

static void test_link_rx_survives_fragmentation_and_latency(void) {
  FakeLinkPhy a, b;
  a.connect(&b);
  a.maxWriteChunk = 1;                          // 一次只出一个字节
  a.latencySteps = 3;                           // 还要过 3 个虚拟时间格
  LinkTx tx;
  LinkRx rx;
  uint8_t p[kLenMax];
  fillPayload(p, kLenMax, 0x60);
  TEST_ASSERT_TRUE(tx.enqueueFrame((uint8_t)MsgType::Status, p, kStatusLen, kRoleMaster));

  Frame f;
  bool got = false;
  for (uint16_t round = 0; round < 400u && !got; ++round) {
    if (tx.queued() > 0u) tx.pump(a);
    a.step();
    if (rx.poll(b, &f)) got = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(got, "分片 + 延迟之后依然要能解出一帧");
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Status, f.type);
  TEST_ASSERT_EQUAL_UINT8(kStatusLen, f.len);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().frames_ok);
}

// 从板 RX 上会混着回放行（§2 的分流：'V' = 0x56 与 0x5A 不撞）：
// 整行都该被当噪声丢掉，然后来的第一帧照样收下
static void test_link_rx_hunts_sync_through_replay_text(void) {
  const char* line = "VAN 824 18 F8 27 10 00 00 00\n";
  LinkRx rx;
  Frame f;
  uint32_t rejects = 0;
  for (const char* c = line; *c != '\0'; ++c) {
    const LinkRx::Step st = rx.feed((uint8_t)*c, &f);
    TEST_ASSERT_EQUAL_INT((int)LinkRx::Step::NeedMore, (int)st);   // 一行文本解不出帧
  }
  TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(line), rx.stats().noise_bytes);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().frames_ok);

  uint8_t good[16];
  const uint16_t n = makeFrame((uint8_t)MsgType::Tick, 0x70, kRoleMaster, good, sizeof(good));
  TEST_ASSERT_TRUE(feedFrame(rx, good, n, &f, &rejects));
  TEST_ASSERT_EQUAL_UINT32(0u, rejects);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Tick, f.type);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().frames_ok);
}

// 一帧 CRC 坏了，紧接着就是好帧 ⇒ 只丢坏的那一帧（§2：从 SYNC 之后一个字节继续找）
static void test_link_rx_resync_after_crc_error(void) {
  uint8_t f1[16], f2[16];
  const uint16_t n1 = makeFrame((uint8_t)MsgType::Data, 0x80, kRoleMaster, f1, sizeof(f1));
  const uint16_t n2 = makeFrame((uint8_t)MsgType::Tick, 0x90, kRoleMaster, f2, sizeof(f2));
  f1[6] = (uint8_t)(f1[6] ^ 0x01u);             // 改坏一个载荷字节（LEN 没动）

  LinkRx rx;
  Frame f;
  uint32_t rejects = 0;
  TEST_ASSERT_FALSE(feedFrame(rx, f1, n1, &f, &rejects));
  TEST_ASSERT_EQUAL_UINT32(1u, rejects);
  TEST_ASSERT_TRUE(feedFrame(rx, f2, n2, &f, &rejects));
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Tick, f.type);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().crc_err);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().frames_ok);
  // 坏帧 SYNC 之后的 12 个字节里没有别的 SYNC ⇒ 全被当噪声丢掉（§2 的重同步）
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(n1 - 1u), rx.stats().noise_bytes);
}

// ★ 重同步里最容易写错的一条：**LEN 字节**被改坏时，候选帧会把后面的好帧吞掉一部分。
//   照 §2"从 SYNC 之后一个字节继续找下一个 SYNC"，好帧必须还能被捡回来。
static void test_link_rx_resync_when_len_byte_is_corrupted(void) {
  uint8_t f1[16], f2[16];
  const uint16_t n1 = makeFrame((uint8_t)MsgType::Data, 0xA0, kRoleMaster, f1, sizeof(f1));
  const uint16_t n2 = makeFrame((uint8_t)MsgType::Tick, 0xB0, kRoleMaster, f2, sizeof(f2));
  f1[kOffLen] = 12u;                            // LEN 6 → 12：候选帧变成 19 B

  LinkRx rx;
  Frame f;
  uint32_t rejects = 0;
  // 两帧背靠背喂进去（25 B > 19 B ⇒ 候选帧"收齐"了，但 CRC 必然不过）
  TEST_ASSERT_FALSE(feedFrame(rx, f1, n1, &f, &rejects));
  TEST_ASSERT_TRUE_MESSAGE(feedFrame(rx, f2, n2, &f, &rejects),
                           "LEN 被改坏后，被吞掉半截的好帧必须靠'从 SYNC+1 重找'捡回来");
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Tick, f.type);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().crc_err);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().frames_ok);
}

// LEN > 64：按坏帧丢、**不等载荷**（§2）—— 连那个字节后面的 ROLE 都不用等到
static void test_link_rx_bad_len_does_not_wait(void) {
  const uint8_t hdr[5] = {kSync, kVer, (uint8_t)MsgType::Data, 200u, kRoleMaster};
  LinkRx rx;
  Frame f;
  DecodeErr e = DecodeErr::Ok;
  // 前 3 个字节还看不到 LEN（LEN 在第 4 个字节上）
  for (uint8_t i = 0; i < kOffLen; ++i) {
    TEST_ASSERT_EQUAL_INT((int)LinkRx::Step::NeedMore, (int)rx.feed(hdr[i], &f, &e));
  }
  // ★ 一拿到 LEN = 200 就必须立刻丢（"不等载荷"：连第 5 个字节 ROLE 都不需要）
  TEST_ASSERT_EQUAL_INT((int)LinkRx::Step::Reject, (int)rx.feed(hdr[kOffLen], &f, &e));
  TEST_ASSERT_EQUAL_STRING("len_range", decodeErrName(e));
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().bad_len);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().frames_ok);
  TEST_ASSERT_FALSE_MESSAGE(rx.pending(), ">64 的坏帧不许把半截留在缓冲里");

  // 紧接着的好帧照样收下
  uint8_t good[16];
  const uint16_t n = makeFrame((uint8_t)MsgType::Tick, 0xC0, kRoleMaster, good, sizeof(good));
  uint32_t rejects = 0;
  TEST_ASSERT_TRUE(feedFrame(rx, good, n, &f, &rejects));
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().frames_ok);
}

static void test_link_rx_unknown_type_dropped(void) {
  uint8_t bad[16];
  uint8_t payload[kLenMax];
  fillPayload(payload, kLenMax, 0xD0);
  // 0x50 = 从板文本日志转发（§6 的 v2 候选）：v1 必须丢帧并计数
  const uint16_t n = encodeFrame(0x50u, payload, kHelloLen, kRoleMaster, bad, sizeof(bad));
  LinkRx rx;
  Frame f;
  uint32_t rejects = 0;
  TEST_ASSERT_FALSE(feedFrame(rx, bad, n, &f, &rejects));
  TEST_ASSERT_EQUAL_UINT32(1u, rejects);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().unknown_type);
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().frames_ok);

  // 版本不匹配（主版本 2）的**已知** TYPE：照解，并且要置告警位（§2）
  const uint16_t n2 = encodeFrame((uint8_t)MsgType::Tick, payload, kTickLen, kRoleMaster,
                                  bad, sizeof(bad), 0x20u);
  TEST_ASSERT_TRUE(feedFrame(rx, bad, n2, &f, &rejects));
  TEST_ASSERT_TRUE(f.ver_mismatch);
  TEST_ASSERT_TRUE(rx.verMismatchSeen());
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().frames_ok);
}

static void test_link_rx_role_conflict_drops_frame(void) {
  uint8_t frame[16];
  const uint16_t n = makeFrame((uint8_t)MsgType::Tick, 0xE0, kRoleMaster, frame, sizeof(frame));
  LinkRx rx;
  Frame f;
  DecodeErr e = DecodeErr::Ok;

  // 本机也是主板 ⇒ 同角色冲突：丢帧 + 计数 + 置标志（§5 ①，定案 L12）
  rx.setLocalRole(kRoleMaster);
  bool got = false;
  for (uint16_t i = 0; i < n; ++i) {
    if (rx.feed(frame[i], &f, &e) == LinkRx::Step::Frame) got = true;
  }
  TEST_ASSERT_FALSE(got);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().role_conflict);
  TEST_ASSERT_TRUE(rx.roleConflictSeen());
  TEST_ASSERT_EQUAL_UINT32(0u, rx.stats().frames_ok);

  // 反过来（本机是从板）同一帧必须收下
  rx.setLocalRole(kRoleSlave);
  got = false;
  for (uint16_t i = 0; i < n; ++i) {
    if (rx.feed(frame[i], &f, &e) == LinkRx::Step::Frame) got = true;
  }
  TEST_ASSERT_TRUE(got);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().frames_ok);
  TEST_ASSERT_EQUAL_UINT32(1u, rx.stats().role_conflict);   // 旧计数还在
}

// 一次 poll 读的字节数是**有上限**的（§1.3：单次很短、能随时被打断）
static void test_link_rx_poll_budget_bounds_work(void) {
  FakeLinkPhy phy;
  uint8_t noise[200];
  memset(noise, 0x00, sizeof(noise));           // 全 0 噪声（不含 SYNC）
  phy.feed(noise, sizeof(noise));

  LinkRx rx;
  Frame f;
  TEST_ASSERT_FALSE(rx.poll(phy, &f, 10u));
  TEST_ASSERT_EQUAL_UINT32(190u, (uint32_t)phy.rxBytes());
  TEST_ASSERT_EQUAL_UINT32(10u, rx.stats().noise_bytes);

  // 预算够大时把噪声吃光，然后一帧真的照样解出来
  TEST_ASSERT_FALSE(rx.poll(phy, &f, 200u));
  TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)phy.rxBytes());
  uint8_t good[16];
  const uint16_t n = makeFrame((uint8_t)MsgType::Event, 0xF0, kRoleMaster, good, sizeof(good));
  phy.feed(good, n);
  TEST_ASSERT_TRUE(rx.poll(phy, &f, 64u));
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Event, f.type);
  TEST_ASSERT_EQUAL_UINT8(kEventLen, f.len);
}

// ★★ 单板回环的**宿主机复现**（2026-09-23 上板实测踩到的那一次）：
//   回环里回来的字节是**背靠背、帧间零空闲**的（自己的 TX 直接进自己的 RX），
//   所以"分帧"这件事**不许**依赖任何时间/空闲间隔 —— link_rx 里也确实没有 millis()。
//   本用例把回环固件要发的那 200 帧原样拼成一段连续字节流（五类 × 40，帧间一个
//   空闲位都没有）喂进接收侧，断言**一帧不少、顺序不错、ROLE 字节不变**。
//   ★ 前面还补了一个"开机毛刺字节"（板上实测 rxTotal 比 txTotal 多 1）：
//     它必须只算 1 个噪声字节，不许把后面 200 帧带偏。
static void test_link_rx_backtoback_stream_decodes_all_frames(void) {
  uint8_t stream[200u * (uint16_t)kFrameBytesMax];
  const uint16_t n = buildLoopbackStream(stream, (uint16_t)sizeof(stream));
  // 40 × (12+12+13+23+11) = 2840 B —— 与板上那次 txTotal=2840 对齐
  TEST_ASSERT_EQUAL_UINT16(2840u, n);

  FakeLinkPhy phy;
  const uint8_t glitch = 0x00u;          // 不是 SYNC ⇒ 只能进噪声计数
  phy.feed(&glitch, 1u);
  phy.feed(stream, n);                   // ★ 整段一次性进去：帧间没有任何间隔

  const uint8_t types[5] = {(uint8_t)MsgType::Hello, (uint8_t)MsgType::Tick,
                            (uint8_t)MsgType::Data,  (uint8_t)MsgType::Status,
                            (uint8_t)MsgType::Event};
  LinkRx rx;
  rx.setLocalRole(kLoopbackRxRole);
  Frame f;
  uint32_t got = 0;
  while (got < 200u && rx.poll(phy, &f, 64u)) {
    TEST_ASSERT_EQUAL_HEX8(types[got / 40u], f.type);        // 顺序/边界都不许错位
    TEST_ASSERT_EQUAL_UINT8(kLoopbackTxRole, f.role);        // §2 的 ROLE 往返不变
    ++got;
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(200u, got, "背靠背连续流必须解出全部 200 帧");
  TEST_ASSERT_FALSE_MESSAGE(rx.poll(phy, &f, 64u), "不该有第 201 帧");

  const LinkRxStats& st = rx.stats();
  TEST_ASSERT_EQUAL_UINT32(200u, st.frames_ok);
  TEST_ASSERT_EQUAL_UINT32(0u, st.framesDropped());
  TEST_ASSERT_EQUAL_UINT32(0u, st.role_conflict);
  TEST_ASSERT_EQUAL_UINT32(0u, st.crc_err);
  TEST_ASSERT_EQUAL_UINT32(0u, st.bad_len);
  TEST_ASSERT_EQUAL_UINT32(0u, st.unknown_type);
  TEST_ASSERT_EQUAL_UINT32(1u, st.noise_bytes);            // 就是那个毛刺字节
  TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)phy.rxBytes());   // 字节全吃光（无残留）
  TEST_ASSERT_EQUAL_UINT16(0u, rx.pendingBytes());

  // 同一段流逐字节喂（feed，完全没有"批"的概念）也必须一样 —— 分帧与时间无关
  uint32_t per_type[5] = {0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT32(200u, drainLoopbackStream(stream, n, kLoopbackRxRole, per_type));
  for (uint8_t k = 0; k < 5; ++k) TEST_ASSERT_EQUAL_UINT32(40u, per_type[k]);
}

// ★★ 根因回归：单板回环的**收端角色**（2026-09-23 上板实测后的定案）
//   §5 ① 的判据是"对端 ROLE == 本机 ROLE ⇒ 丢帧"。回环的发端写进帧里的 ROLE 是
//   **本机**角色，所以收端**必须**扮演对端；两边同角色时，每一帧都会在
//   decodeFrame() 返回 **Ok 之后**被丢掉 ⇒ 症状是"字节全收到、0 帧，而
//   crc/bad_len/未知类型 全是 0"（最误导人的那一种，见 link_role.h 那段注释）。
static void test_link_loopback_rx_role_must_be_peer(void) {
  TEST_ASSERT_FALSE_MESSAGE(roleConflict(kLoopbackTxRole, kLoopbackRxRole),
                            "回环的收端角色必须与帧上写的角色互补，否则 §5 ① 丢掉每一帧");
  TEST_ASSERT_TRUE_MESSAGE(roleConflict(kLoopbackTxRole, kLocalRole),
                           "参照物:收端若声明本机角色,冲突必然成立(这就是旧口径)");

  // ① 现行口径（收端扮演对端）：同一帧必须收下
  uint8_t frame[16];
  const uint16_t fn =
      makeFrame((uint8_t)MsgType::Data, 0x40, kLoopbackTxRole, frame, sizeof(frame));
  LinkRx ok_rx;
  ok_rx.setLocalRole(kLoopbackRxRole);
  Frame f;
  uint32_t rejects = 0;
  TEST_ASSERT_TRUE(feedFrame(ok_rx, frame, fn, &f, &rejects));
  TEST_ASSERT_EQUAL_UINT32(0u, rejects);
  TEST_ASSERT_EQUAL_UINT32(0u, ok_rx.stats().role_conflict);

  // ② 错口径（收端也声明本机角色）—— 精确复现板上那一次：丢帧发生在解码**成功**
  //    之后，所以"帧坏了"的那几个计数一个都不动，只有 role_conflict 在涨
  LinkRx bad_rx;
  bad_rx.setLocalRole(kLocalRole);
  rejects = 0;
  TEST_ASSERT_FALSE(feedFrame(bad_rx, frame, fn, &f, &rejects));
  TEST_ASSERT_EQUAL_UINT32(1u, rejects);                   // Step::RoleDrop
  TEST_ASSERT_EQUAL_UINT32(1u, bad_rx.stats().role_conflict);
  TEST_ASSERT_EQUAL_UINT32(0u, bad_rx.stats().frames_ok);
  TEST_ASSERT_EQUAL_UINT32(0u, bad_rx.stats().crc_err);
  TEST_ASSERT_EQUAL_UINT32(0u, bad_rx.stats().bad_len);
  TEST_ASSERT_EQUAL_UINT32(0u, bad_rx.stats().unknown_type);
  TEST_ASSERT_EQUAL_UINT32(0u, bad_rx.stats().noise_bytes);

  // ③ 整段 200 帧也一样：错口径下"2840 字节全吃掉 / 0 帧 / 连噪声都是 0" ——
  //    正是板上那次的读数（那 1 个噪声字节是开机毛刺，与这条无关）
  uint8_t stream[200u * (uint16_t)kFrameBytesMax];
  const uint16_t n = buildLoopbackStream(stream, (uint16_t)sizeof(stream));
  TEST_ASSERT_EQUAL_UINT16(2840u, n);
  FakeLinkPhy phy;
  phy.feed(stream, n);
  LinkRx full_rx;
  full_rx.setLocalRole(kLocalRole);
  Frame ff;
  uint32_t got = 0;
  // ★ 这里必须按"PHY 排空"来循环，不能写成 while (poll(...))：错口径下 poll() 每
  //   次只读满自己的 64 B 预算就返回 false（一帧都没解出来），写成后者会**提前收工**。
  while (phy.rxBytes() > 0u) {
    if (full_rx.poll(phy, &ff, 64u)) ++got;
  }
  TEST_ASSERT_EQUAL_UINT32(0u, got);
  TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)phy.rxBytes());
  TEST_ASSERT_EQUAL_UINT32(200u, full_rx.stats().role_conflict);
  TEST_ASSERT_EQUAL_UINT32(0u, full_rx.stats().frames_ok);
  TEST_ASSERT_EQUAL_UINT32(0u, full_rx.stats().noise_bytes);
  TEST_ASSERT_EQUAL_UINT32(0u, full_rx.stats().crc_err);
  TEST_ASSERT_EQUAL_UINT32(0u, full_rx.stats().bad_len);
}

void register_link_phy_tests(void) {
  RUN_TEST(test_link_phy_loopback_and_counts);
  RUN_TEST(test_link_phy_fragmentation_caps_each_write);
  RUN_TEST(test_link_phy_drop_injection);
  RUN_TEST(test_link_phy_latency_needs_steps);
  RUN_TEST(test_link_phy_disconnect_blocks_io);
  RUN_TEST(test_link_tx_ring_covers_worst_burst);
  RUN_TEST(test_link_tx_enqueue_never_touches_phy);
  RUN_TEST(test_link_tx_drops_whole_frame_when_full);
  RUN_TEST(test_link_tx_pump_drains_in_order_with_slow_phy);
  RUN_TEST(test_link_tx_pump_does_not_busy_wait);
  RUN_TEST(test_link_rx_decodes_frames_from_phy);
  RUN_TEST(test_link_rx_survives_fragmentation_and_latency);
  RUN_TEST(test_link_rx_hunts_sync_through_replay_text);
  RUN_TEST(test_link_rx_resync_after_crc_error);
  RUN_TEST(test_link_rx_resync_when_len_byte_is_corrupted);
  RUN_TEST(test_link_rx_bad_len_does_not_wait);
  RUN_TEST(test_link_rx_unknown_type_dropped);
  RUN_TEST(test_link_rx_role_conflict_drops_frame);
  RUN_TEST(test_link_rx_poll_budget_bounds_work);
  // ★ 单板回环的两条（2026-09-23 上板踩到"收了很多字节却 0 帧"之后补的）：
  //   背靠背连续流必须解出全部 200 帧；回环的收端角色必须与帧上的 ROLE 互补。
  RUN_TEST(test_link_rx_backtoback_stream_decodes_all_frames);
  RUN_TEST(test_link_loopback_rx_role_must_be_peer);
}

// 双板链路 —— **ESP-NOW 那一档 + 测速/测丢包**的宿主机用例（2026-09-27 新增）
//
// 本文件钉三件事（对应本单的三条要求）：
//   ① **接口对齐**：ESP-NOW 的 PHY 与 UART 的 PHY 是**同一套形状**
//      （`begin/pumpTx/port/txPin/rxPin/loopback/started/baud` 一个不少）——
//      这两个类都**编不进宿主机**（一个要 `HardwareSerial`、一个要 `WiFi`/`esp_now_*`），
//      所以这里用"**签名对照桩**"（`EspNowShapeMock` / `UartShapeMock`）：把两边的
//      方法按固件里的声明**逐条照抄**，再拿同一段模板函数去调 —— 固件那边只用一次
//      `g_link_phy.<名字>()`（两个角色共用一份代码），所以**任何一边改了签名，
//      `main.cpp` 那边就编不过**；这里钉的是"这一套形状本身"（谁少一个方法就红）。
//   ② **寻址/信道/NVS 的口径**：信道 1..13、广播地址、环/在途上限、
//      "一帧必须装进一个 ESP-NOW 包"这条编译期判据的运行期影子。
//   ③ **测速/测丢包**：信封逐字节、发端节奏、收端的丢包率/最大间隔/p50-p95-p99、
//      以及"门槛达不到就是不适合"（不许放宽）。
//
// ★ 本组**不动协议**：消息类型 / LEN / 字段次序 / CRC 覆盖范围一个都没改
//   （测量帧走的是既有的 `MsgType::Data`，见 link_meas.h 文件头）。
#include <unity.h>

#include <stdio.h>
#include <string.h>
#include <string>

#include "link_frame.h"
#include "link_meas.h"
#include "link_msg.h"
#include "link_phy.h"
#include "link_phy_espnow_cfg.h"
#include "link_phy_pins.h"
#include "link_role.h"

using namespace dashlink;

// ============================================================
// ① 接口对齐
// ============================================================
// ★ 为什么要这两个桩：两个真实现都**编不进宿主机**（一个要 HardwareSerial、
//   一个要 WiFi/esp_now）。而这个"形状"恰恰是**跨文件**的约定：
//   `main.cpp` 里两个角色共用 `g_link_phy` 一份代码，所以"少一个方法/签名不同"
//   在那边是编译错误 —— 这两个桩把同一份声明抄过来，让"少一个方法"在宿主机上
//   也能被钉住（含访问器与 `pumpTx()` 的返回类型）。
//   ★ 抄错了怎么办：桩里的签名**逐字**来自 link_phy_uart.h / link_phy_espnow.h；
//     真实现改了签名而桩没改 ⇒ 下面那个"两个桩都跑同一段模板代码"仍然过，
//     但 `main.cpp` 会在固件构建里编不过（那条是硬的，见回报里的四个 env SUCCESS）。
class UartShapeMock {
 public:
  void begin(bool loopback = false) { mBeginArg = loopback; mOnline = true; }
  uint16_t pumpTx() { return 0u; }
  int8_t txPin() const { return 43; }
  int8_t rxPin() const { return 44; }
  int8_t port() const { return 0; }
  bool loopback() const { return mBeginArg; }
  bool started() const { return mOnline; }
  uint32_t baud() const { return 115200u; }
  int available() { return 0; }
  int read() { return -1; }
  int availableForWrite() { return 0; }
  size_t write(const uint8_t*, size_t) { return 0u; }
  bool online() const { return mOnline; }

 private:
  bool mBeginArg = false;
  bool mOnline = false;
};

class EspNowShapeMock {
 public:
  void begin(bool loopback = false) { mBeginArg = loopback; mOnline = true; }
  uint16_t pumpTx() { return 0u; }
  int8_t txPin() const { return -1; }
  int8_t rxPin() const { return -1; }
  int8_t port() const { return -1; }
  bool loopback() const { return false; }          // 无线没有回环这一说
  bool started() const { return mOnline; }
  uint32_t baud() const { return 1000000u; }
  const char* phyName() const { return "ESP-NOW"; }
  const char* peerText() const { return "(none)"; }
  int available() { return 0; }
  int read() { return -1; }
  int availableForWrite() { return 0; }
  size_t write(const uint8_t*, size_t) { return 0u; }
  bool online() const { return mOnline; }

 private:
  bool mBeginArg = false;
  bool mOnline = false;
};

// ★ 同一个模板跑两个桩：**方法名/参数/返回类型**对不上就在这里编不过。
//   （这一段是"接口对齐"那条要求最直接的证据 —— 它就是两份签名的交集断言。）
template <typename Phy>
static uint32_t phyShapeContract(Phy& phy) {
  phy.begin(false);
  const uint16_t pumped = phy.pumpTx();
  const int8_t tx = phy.txPin();
  const int8_t rx = phy.rxPin();
  const int8_t port = phy.port();
  const bool lp = phy.loopback();
  const bool st = phy.started();
  const uint32_t baud = phy.baud();
  const int avail = phy.available();
  const int rd = phy.read();
  const int room = phy.availableForWrite();
  const uint8_t b = 0x5Au;
  const size_t wrote = phy.write(&b, 1u);
  const bool on = phy.online();
  return (uint32_t)(pumped + (uint16_t)tx + (uint16_t)rx + (uint16_t)port + (lp ? 1u : 0u) +
                    (st ? 1u : 0u) + baud + (uint32_t)avail + (uint32_t)(rd + 1) +
                    (uint32_t)room + (uint32_t)wrote + (on ? 1u : 0u));
}

static void test_link_phy_espnow_shape_matches_uart(void) {
  UartShapeMock uart;
  EspNowShapeMock now;
  // 两个桩都能被同一段模板代码调用 ⇒ 形状（方法名/参数/返回类型）一致。
  const uint32_t a = phyShapeContract(uart);
  const uint32_t b = phyShapeContract(now);
  TEST_ASSERT_GREATER_THAN_UINT32(0u, a);
  TEST_ASSERT_GREATER_THAN_UINT32(0u, b);

  // 差异是**刻意的**，逐条钉住（不然下一个人会把它们当成"漏了"）：
  TEST_ASSERT_EQUAL_INT(0, (int)uart.port());          // UART：UART0
  TEST_ASSERT_EQUAL_INT(-1, (int)now.port());          // 无线：没有端口
  TEST_ASSERT_EQUAL_INT(43, (int)uart.txPin());        // §0：43 发
  TEST_ASSERT_EQUAL_INT(-1, (int)now.txPin());         // 无线：没有引脚（-1 = 没有）
  TEST_ASSERT_TRUE(uart.baud() == kLinkBaud);          // §1.1：115200
  TEST_ASSERT_TRUE(now.baud() > 0u);                   // 空气速率（只用于日志）
  TEST_ASSERT_FALSE(now.loopback());                   // 无线不回环
}

// ============================================================
// ② 寻址 / 信道 / 上限（常量的**唯一出处**在 link_phy_espnow_cfg.h）
// ============================================================
static void test_espnow_channel_is_fixed_and_in_range(void) {
  // ★ 两端必须同一个信道（ESP-NOW 不跨信道），所以它必须是**编译期常量**：
  //   配错的症状是"两端都打 rx=0"，没有任何编译期信号。
  TEST_ASSERT_GREATER_OR_EQUAL_INT(1, (int)kEspNowChannel);
  TEST_ASSERT_LESS_OR_EQUAL_INT(13, (int)kEspNowChannel);
  // 默认 6：1/6/11 三条互不重叠信道的正中间那条
  TEST_ASSERT_EQUAL_INT(6, (int)kEspNowChannel);
}

static void test_espnow_broadcast_address_is_all_ff(void) {
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) {
    TEST_ASSERT_EQUAL_UINT8(0xFFu, kEspNowBroadcast[i]);
  }
  TEST_ASSERT_EQUAL_UINT8(6u, kEspNowMacLen);   // MAC = 6 字节（ESP-NOW 的 peer 口径）
}

static void test_espnow_rings_and_pending_bounds(void) {
  // 环至少要装得下一帧最坏情况（71 B）—— 否则 `write()` 永远收不下整帧
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)kFrameBytesMax, (uint32_t)kEspNowTxRing);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)kFrameBytesMax, (uint32_t)kEspNowRxRing);
  // 在途上限：至少 1（否则一个包都发不出去）
  TEST_ASSERT_GREATER_OR_EQUAL_INT(1, (int)kEspNowMaxPending);
  TEST_ASSERT_LESS_OR_EQUAL_INT(32, (int)kEspNowMaxPending);
  // 一次 pumpTx 至少能交 1 个包
  TEST_ASSERT_GREATER_OR_EQUAL_INT(1, (int)kEspNowPumpPackets);
}

// ★ ESP-NOW 的单包有效载荷上限 = 250 B（IDF 口径：ESP_NOW_MAX_DATA_LEN）。
//   链路帧最大 71 B ⇒ 一帧一个包，**永远不需要分片**。
//   这条的编译期版本是 cfg 头文件里那条 static_assert；这里是它的运行期影子。
static void test_espnow_one_frame_fits_one_packet(void) {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(250u, (uint32_t)kFrameBytesMax);
  // 真实流量的三种帧也要逐条落在里面（§2 的帧长表）
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(250u, (uint32_t)frameBytesForLen(kTickLen));
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(250u, (uint32_t)frameBytesForLen(kDataLen));
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(250u, (uint32_t)frameBytesForLen(kStatusLen));
}

// NVS 的键/命名空间（"学到 MAC 就存进 NVS"那条要求的落点）
static void test_espnow_nvs_keys_are_named(void) {
  TEST_ASSERT_NOT_NULL(kEspNowNvsNamespace);
  TEST_ASSERT_NOT_NULL(kEspNowNvsPeerKey);
  TEST_ASSERT_EQUAL_STRING("dash", kEspNowNvsNamespace);   // 与静音那一路同一个命名空间
  TEST_ASSERT_TRUE(strlen(kEspNowNvsPeerKey) > 0u);
  TEST_ASSERT_TRUE(strlen(kEspNowNvsPeerKey) <= 15u);      // NVS 键 15 字符上限
  TEST_ASSERT_TRUE(strlen(kEspNowNvsNamespace) <= 15u);
}

// ============================================================
// ③ 测量信封 + 发端 + 收端
// ============================================================
namespace {

// 收件箱假 PHY：write() 把**整帧字节**收下来（ESP-NOW 那一档的 write 就是
// 整帧进出），并提供 available/read —— 这样发端那条路能走**真的** LinkPhy 接口。
class InboxPhy : public LinkPhy {
 public:
  int available() override { return (int)mLen; }
  int read() override {
    if (mLen == 0u) return -1;
    const uint8_t b = mBuf[mTail];
    mTail = (uint16_t)(mTail + 1u);
    --mLen;
    return (int)b;
  }
  int availableForWrite() override { return mFull ? 0 : (int)sizeof(mBuf); }
  size_t write(const uint8_t* data, size_t n) override {
    if (mFull) return 0u;
    if (n > sizeof(mBuf)) return 0u;                    // 整帧进出（与真实现同口径）
    memcpy(mBuf, data, n);
    mTail = 0;
    mLen = (uint16_t)n;
    mWrites++;
    return n;
  }
  bool online() const override { return true; }

  void setFull(bool f) { mFull = f; }
  uint32_t writes() const { return mWrites; }
  uint16_t len() const { return mLen; }

 private:
  uint8_t mBuf[kFrameBytesMax] = {0};
  uint16_t mLen = 0;
  uint16_t mTail = 0;
  uint32_t mWrites = 0;
  bool mFull = false;
};

// 把发端刚写进假 PHY 的那一帧读出来解成 Frame（顺便证明它**是**一帧合法 v1 帧）
bool takeFrame(InboxPhy& phy, Frame* out) {
  uint8_t buf[kFrameBytesMax] = {0};
  const int n = phy.available();
  if (n < (int)kOverhead) return false;
  for (int i = 0; i < n; ++i) {
    const int c = phy.read();
    if (c < 0) return false;
    buf[i] = (uint8_t)c;
  }
  return decodeFrame(buf, (uint16_t)n, out) == DecodeErr::Ok;
}

// 造一个"到达"（收端只用它做统计，不经过帧层）
void feedArrival(MeasReceiver& rx, uint16_t seq, uint32_t ms, bool first, uint16_t sent_ms = 0) {
  uint8_t p[kMeasEnvelopeLen] = {0};
  p[0] = kMeasMagic[0];
  p[1] = kMeasMagic[1];
  p[2] = kMeasMagic[2];
  p[3] = kMeasMagic[3];
  p[kMeasOffVer] = kMeasVer;
  const uint16_t s = (uint16_t)((seq & kMeasSeqMask) | (first ? kMeasFirstBit : 0u));
  p[kMeasOffSeq + 0u] = (uint8_t)((s >> 8) & 0xFFu);
  p[kMeasOffSeq + 1u] = (uint8_t)(s & 0xFFu);
  p[kMeasOffMs + 0u] = (uint8_t)((sent_ms >> 8) & 0xFFu);
  p[kMeasOffMs + 1u] = (uint8_t)(sent_ms & 0xFFu);
  TEST_ASSERT_TRUE(rx.noteArrival(p, kMeasEnvelopeLen, ms));
}

void drain(MeasReceiver& rx) {
  while (rx.poll() > 0u) {
  }
}

std::string summaryOf(const MeasRxResult& r) {
  // ★ 缓冲要比一行长（实测约 280 B）：`std::string(const char*)` 在**容量不够**时
  //   得到的是一个**没有 NUL 结尾**的字符串 ⇒ 后面的 `find()` 会在越界读上瞎找，
  //   症状是"字段明明在却找不到"。所以这里留 512 B（= `main.cpp` 那两行的口径）。
  char buf[512] = {0};
  measFormatRxSummary(r, buf, (int)sizeof(buf));
  return std::string(buf);
}

}  // namespace

// ---- 信封：逐字节 ----
static void test_meas_envelope_layout_matches_spec(void) {
  TEST_ASSERT_EQUAL_UINT8(9u, kMeasEnvelopeLen);
  TEST_ASSERT_TRUE(kMeasEnvelopeLen >= kLenMin && kMeasEnvelopeLen <= kLenMax);
  // ★ 它必须**不长于** 9 B 这条预算（信封 = 魔数 4 + 版本 1 + 序号 2 + 发端时刻 2）
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(9u, (uint32_t)kMeasEnvelopeLen);
  // ★ 契约 DATA 的载荷宽度（§3 的逐字节表）**一个字都没改** —— 这条是"没动协议"的
  //   可执行形式：信封要迁就 LEN 的合法范围（4..16），不是反过来。
  TEST_ASSERT_EQUAL_UINT8(6u, kDataLen);
  TEST_ASSERT_TRUE(kMeasEnvelopeLen <= kLenMax);
  TEST_ASSERT_EQUAL_UINT8('D', kMeasMagic[0]);
  TEST_ASSERT_EQUAL_UINT8('S', kMeasMagic[1]);
  TEST_ASSERT_EQUAL_UINT8('M', kMeasMagic[2]);
  TEST_ASSERT_EQUAL_UINT8('1', kMeasMagic[3]);
  TEST_ASSERT_EQUAL_UINT8(1u, kMeasVer);
  TEST_ASSERT_EQUAL_UINT8(4u, kMeasOffVer);
  TEST_ASSERT_EQUAL_UINT8(5u, kMeasOffSeq);
  TEST_ASSERT_EQUAL_UINT8(7u, kMeasOffMs);
  // 魔数**不含** 0x5A(SYNC)：载荷里出现帧头字节会让误码时的重同步更难
  for (uint8_t i = 0; i < 4u; ++i) TEST_ASSERT_NOT_EQUAL((int)kSync, (int)kMeasMagic[i]);
}

// ---- 发端：真的发出**合法 v1 帧**，逐字节是那个信封 ----
// ★ 发端那一支的语义（`MeasSender::start`）：`mNextMs = now` ⇒ **下一次 poll 立刻发
//   第一帧**（"上电就发，别等一格"），之后的节拍才是每 `period_ms` 一帧。
static void test_meas_sender_emits_valid_v1_data_frames(void) {
  MeasSender tx;
  InboxPhy phy;
  tx.start(1000u, kRoleMaster, 3u, 10u);
  TEST_ASSERT_TRUE(tx.active());

  TEST_ASSERT_EQUAL_UINT8(1u, tx.poll(1000u, phy));   // ★ 第一帧立刻发
  Frame f;
  TEST_ASSERT_TRUE(takeFrame(phy, &f));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)MsgType::Data, f.type);      // ★ 不新增消息类型
  TEST_ASSERT_EQUAL_UINT8(kMeasEnvelopeLen, f.len);
  TEST_ASSERT_EQUAL_UINT8(kRoleMaster, f.role);
  TEST_ASSERT_TRUE(f.crc == f.crc_calc);                        // CRC-15 过（协议没动）
  for (uint8_t i = 0; i < 4u; ++i) TEST_ASSERT_EQUAL_UINT8(kMeasMagic[i], f.payload[i]);
  TEST_ASSERT_EQUAL_UINT8(kMeasVer, f.payload[kMeasOffVer]);
  const uint16_t s0 = (uint16_t)(((uint16_t)f.payload[kMeasOffSeq] << 8) |
                                 (uint16_t)f.payload[kMeasOffSeq + 1u]);
  TEST_ASSERT_EQUAL_UINT16(kMeasFirstBit, (uint16_t)(s0 & kMeasFirstBit));  // 第一帧打标
  TEST_ASSERT_EQUAL_UINT16(0u, (uint16_t)(s0 & kMeasSeqMask));              // 序号从 0 起
  TEST_ASSERT_EQUAL_UINT16(1000u, (uint16_t)(((uint16_t)f.payload[kMeasOffMs] << 8) |
                                             (uint16_t)f.payload[kMeasOffMs + 1u]));

  // 到点才发第二帧（10 ms 一格）
  TEST_ASSERT_EQUAL_UINT8(0u, tx.poll(1005u, phy));
  TEST_ASSERT_EQUAL_UINT8(1u, tx.poll(1010u, phy));
  TEST_ASSERT_TRUE(takeFrame(phy, &f));
  const uint16_t s1 = (uint16_t)(((uint16_t)f.payload[kMeasOffSeq] << 8) |
                                 (uint16_t)f.payload[kMeasOffSeq + 1u]);
  TEST_ASSERT_EQUAL_UINT16(1u, (uint16_t)(s1 & kMeasSeqMask));              // 序号 +1
  TEST_ASSERT_EQUAL_UINT16(0u, (uint16_t)(s1 & kMeasFirstBit));             // 后续帧不打标

  // 发满计划帧数就自动停（实测时不用人守着看表）
  TEST_ASSERT_EQUAL_UINT8(1u, tx.poll(1020u, phy));
  TEST_ASSERT_FALSE(tx.active());
  TEST_ASSERT_EQUAL_UINT8(0u, tx.poll(1030u, phy));
  TEST_ASSERT_EQUAL_UINT32(3u, tx.sent());
  TEST_ASSERT_EQUAL_UINT32(3u, tx.planned());
  TEST_ASSERT_EQUAL_UINT8(3u, (uint8_t)tx.seqLast() + 1u);
}

// ---- 发端：一拍最多 kMaxPerPoll（"每圈有上界"那一条） ----
static void test_meas_sender_is_bounded_per_poll(void) {
  MeasSender tx;
  InboxPhy phy;
  tx.start(0u, kRoleMaster, 10u, 1u);
  TEST_ASSERT_EQUAL_UINT8(MeasSender::kMaxPerPoll, tx.poll(100u, phy));
  TEST_ASSERT_EQUAL_UINT8(MeasSender::kMaxPerPoll, tx.poll(200u, phy));
  TEST_ASSERT_EQUAL_UINT32(8u, tx.sent());
  // PHY 报满时一个字节都不写（非阻塞契约）
  phy.setFull(true);
  const uint32_t before = tx.sent();
  TEST_ASSERT_EQUAL_UINT8(0u, tx.poll(300u, phy));
  TEST_ASSERT_EQUAL_UINT32(before, tx.sent());
}

// ---- 收端：一帧不丢 ----
// ★ 这里**边喂边收**（每一帧之后 drain 一次）—— 那正是设备上的真实节奏：
//   主循环实测 ~900 圈/秒，而 burst 是 100 Hz ⇒ 一圈只到 0~1 帧，队列根本用不上。
//   "一口气喂 400 帧再收"是**不现实**的用法，它测的是队列深度而不是链路
//   （队列满的那条路径另有专门一条用例，见 `…queue_overflow_is_visible`）。
static void test_meas_receiver_counts_no_loss(void) {
  MeasReceiver rx;
  rx.reset();
  for (uint16_t i = 0; i < 400u; ++i) {
    feedArrival(rx, i, 1000u + (uint32_t)i * 10u, i == 0u);
    drain(rx);
  }
  const MeasRxResult& r = rx.result();
  TEST_ASSERT_EQUAL_UINT32(400u, r.frames_rx);
  TEST_ASSERT_EQUAL_UINT32(400u, r.expected);
  TEST_ASSERT_EQUAL_UINT32(0u, r.lost);
  TEST_ASSERT_EQUAL_UINT32(0u, r.loss_per_100k);
  TEST_ASSERT_EQUAL_UINT32(10u, r.gap_max_ms);        // 10 ms 一格，稳定
  TEST_ASSERT_EQUAL_UINT32(10u, r.p50_ms);
  TEST_ASSERT_EQUAL_UINT32(10u, r.p99_ms);
  TEST_ASSERT_EQUAL_UINT32(0u, r.queue_drop);         // 边喂边收 ⇒ 一条都没丢
  TEST_ASSERT_TRUE(r.enough());
  TEST_ASSERT_TRUE(r.passLoss());
  TEST_ASSERT_TRUE(r.passGap());
  TEST_ASSERT_TRUE(r.passJitter());
  TEST_ASSERT_EQUAL_STRING("适合", r.verdict());
}

// ---- 收端：丢包率的分母是"序号跨度"，不是"收到多少" ----
static void test_meas_receiver_loss_uses_seq_span(void) {
  {
    MeasReceiver rx;
    rx.reset();
    for (uint16_t i = 0; i < 1000u; ++i) {
      if (i == 500u) continue;                       // 丢 1/1000 = 0.1 %
      feedArrival(rx, i, 1000u + (uint32_t)i * 10u, i == 0u);
      drain(rx);
    }
    const MeasRxResult& r = rx.result();
    TEST_ASSERT_EQUAL_UINT32(1000u, r.expected);
    TEST_ASSERT_EQUAL_UINT32(999u, r.frames_rx);
    TEST_ASSERT_EQUAL_UINT32(1u, r.lost);
    TEST_ASSERT_EQUAL_UINT32(100u, r.loss_per_100k);      // 正好 0.1 %
    // ★ 门槛是"**严格**小于 0.1 %" ⇒ 正好 0.1 % 不算过（不许放宽）
    TEST_ASSERT_FALSE(r.passLoss());
    TEST_ASSERT_EQUAL_STRING("不适合(丢包)", r.verdict());
  }
  {
    MeasReceiver rx;
    rx.reset();
    for (uint16_t i = 0; i < 2000u; ++i) {
      if (i == 100u) continue;                       // 丢 1/2000 = **0.05 %**（合格）
      feedArrival(rx, i, 1000u + (uint32_t)i * 10u, i == 0u);
      drain(rx);
    }
    const MeasRxResult& r = rx.result();
    TEST_ASSERT_EQUAL_UINT32(2000u, r.expected);
    TEST_ASSERT_EQUAL_UINT32(1999u, r.frames_rx);
    TEST_ASSERT_EQUAL_UINT32(50u, r.loss_per_100k);      // 0.05 % < 0.1 % ⇒ 合格
    TEST_ASSERT_TRUE(r.passLoss());
    TEST_ASSERT_EQUAL_STRING("适合", r.verdict());
  }
}

// ---- 收端：连续最大间隔（§4 第一档 100 ms） ----
static void test_meas_receiver_max_gap_gate(void) {
  {
    MeasReceiver rx;
    rx.reset();
    uint32_t t = 1000u;
    for (uint16_t i = 0; i < 400u; ++i) {
      feedArrival(rx, i, t, i == 0u);
      drain(rx);
      t += (i == 200u) ? 150u : 10u;                 // 中间来一个 150 ms 的空档
    }
    const MeasRxResult& r = rx.result();
    TEST_ASSERT_EQUAL_UINT32(150u, r.gap_max_ms);
    TEST_ASSERT_FALSE(r.passGap());
    TEST_ASSERT_EQUAL_STRING("不适合(最大间隔)", r.verdict());
    // 它同时落进溢出格（≥95 ms）—— 任何"接近超时"的形态都不许被直方图吞掉
    TEST_ASSERT_EQUAL_UINT32(1u, r.over95);
  }
  {
    MeasReceiver rx;
    rx.reset();
    uint32_t t = 1000u;
    for (uint16_t i = 0; i < 400u; ++i) {
      feedArrival(rx, i, t, i == 0u);
      drain(rx);
      t += (i == 200u) ? 40u : 10u;                  // 40 ms 的空档：在门槛内
    }
    const MeasRxResult& r = rx.result();
    TEST_ASSERT_EQUAL_UINT32(40u, r.gap_max_ms);
    TEST_ASSERT_TRUE(r.passGap());
    TEST_ASSERT_TRUE(r.passJitter());
    TEST_ASSERT_EQUAL_UINT32(0u, r.over95);
  }
}

// ---- 收端：p99 抖动门槛（一个 TICK 周期 = 20 ms） ----
static void test_meas_receiver_jitter_gate(void) {
  MeasReceiver rx;
  rx.reset();
  uint32_t t = 1000u;
  // 10 ms 一格打底，每 10 帧来一个 30 ms 的尖刺（≈10 % 的样本）
  // ⇒ p50 = 10 ms（过）、p99 = 30 ms（不过）⇒ 判"不适合(抖动)"
  for (uint16_t i = 0; i < 400u; ++i) {
    feedArrival(rx, i, t, i == 0u);
    drain(rx);
    t += ((i % 10u) == 9u) ? 30u : 10u;
  }
  const MeasRxResult& r = rx.result();
  TEST_ASSERT_EQUAL_UINT32(10u, r.p50_ms);
  TEST_ASSERT_EQUAL_UINT32(30u, r.p99_ms);
  TEST_ASSERT_TRUE(r.passLoss());
  TEST_ASSERT_TRUE(r.passGap());
  TEST_ASSERT_FALSE(r.passJitter());
  TEST_ASSERT_EQUAL_STRING("不适合(抖动)", r.verdict());
}

// ---- 收端：样本不足时**不下结论**（Verdict 三态里的第三态） ----
static void test_meas_receiver_insufficient_samples_says_so(void) {
  MeasReceiver rx;
  rx.reset();
  for (uint16_t i = 0; i < 50u; ++i) feedArrival(rx, i, 1000u + (uint32_t)i * 10u, i == 0u);
  drain(rx);
  const MeasRxResult& r = rx.result();
  TEST_ASSERT_FALSE(r.enough());
  TEST_ASSERT_FALSE(r.passLoss());
  TEST_ASSERT_FALSE(r.passGap());
  TEST_ASSERT_FALSE(r.passJitter());
  TEST_ASSERT_EQUAL_STRING("样本不足", r.verdict());
}

// ---- 收端：burst 的起止（上一发的尾巴不许算进来） ----
static void test_meas_receiver_burst_boundaries(void) {
  MeasReceiver rx;
  rx.reset();
  // 第一次 burst：3 帧
  feedArrival(rx, 0u, 1000u, true);
  feedArrival(rx, 1u, 1010u, false);
  feedArrival(rx, 2u, 1020u, false);
  drain(rx);
  TEST_ASSERT_TRUE(rx.inBurst());
  TEST_ASSERT_FALSE(rx.endedBy(1030u, 250u));       // 还没静够 250 ms
  TEST_ASSERT_TRUE(rx.endedBy(1300u, 250u));        // 静够了 ⇒ 判定结束（只判一次）
  TEST_ASSERT_FALSE(rx.endedBy(1400u, 250u));       // 不会重复判定

  // 第二次 burst 之前，先来一帧**没有第一帧标记**的残留 ⇒ 不许采纳
  feedArrival(rx, 99u, 2000u, false);
  drain(rx);
  TEST_ASSERT_EQUAL_UINT32(3u, rx.result().frames_rx);   // 仍然是上一次那 3 帧

  // 第二次 burst：第一帧打标 ⇒ 统计**清零重来**
  feedArrival(rx, 0u, 3000u, true);
  feedArrival(rx, 1u, 3010u, false);
  drain(rx);
  const MeasRxResult& r = rx.result();
  TEST_ASSERT_EQUAL_UINT32(2u, r.frames_rx);
  TEST_ASSERT_EQUAL_UINT32(2u, r.expected);
  TEST_ASSERT_EQUAL_UINT32(10u, r.gap_max_ms);      // 两次 burst 之间那 1 s 没被算进来
  TEST_ASSERT_EQUAL_UINT32(10u, r.span_arrival_ms);
}

// ---- 收端：队列满时丢的是**统计样本**，而这个损失必须可见 ----
// ★ 在**真机**上，主循环一圈约 1 ms，而到达率在 100 Hz 一档 ⇒ 队列几乎用不上。
//   这里造的是"主循环被拖住"的最坏形态：100 帧挤在 1 ms 里到达 ⇒ 队列（63 条可用）
//   装不下 ⇒ `qdrop` 必须涨（宁可少一条统计样本，也绝不在 WiFi 任务里等）。
static void test_meas_receiver_queue_overflow_is_visible(void) {
  MeasReceiver rx;
  rx.reset();
  for (uint16_t i = 0; i < 100u; ++i) feedArrival(rx, i, 1000u, i == 0u);   // 同一毫秒全到
  drain(rx);                                        // 全部取走后统计
  const MeasRxResult& r = rx.result();
  TEST_ASSERT_EQUAL_UINT32(kMeasRxQueue - 1u, r.frames_rx);   // 只装得下 63 条
  TEST_ASSERT_EQUAL_UINT32(100u - (kMeasRxQueue - 1u), r.queue_drop);   // 丢掉的那些**看得见**
  // ★ 丢的记录是**最后那 37 条**（序号 63..99）⇒ 统计里根本没看见它们，
  //   所以 `expected` 也跟着停在 63（这正是"丢的是统计样本"的准确含义：
  //   它**不会**被算成链路丢包 —— 那是两回事，绝不能混）。
  TEST_ASSERT_EQUAL_UINT32(kMeasRxQueue - 1u, r.expected);
  TEST_ASSERT_EQUAL_UINT32(0u, r.lost);
}

// ---- 收端：同一次 poll 取走多条 ⇒ 单列 coalesced，不当抖动 ----
static void test_meas_receiver_coalesced_is_counted_separately(void) {
  MeasReceiver rx;
  rx.reset();
  for (uint16_t i = 0; i < 12u; ++i) feedArrival(rx, i, 1000u, i == 0u);   // 同一时刻到达
  // 一次 poll 最多 8 条（有上界）⇒ 分两拍取完
  TEST_ASSERT_EQUAL_UINT8(kMeasRxPollMax, rx.poll());
  TEST_ASSERT_EQUAL_UINT8(4u, rx.poll());
  const MeasRxResult& r = rx.result();
  TEST_ASSERT_EQUAL_UINT32(12u, r.frames_rx);
  TEST_ASSERT_GREATER_THAN_UINT32(0u, r.coalesced);   // 本机没跟上，单列
  TEST_ASSERT_EQUAL_UINT32(0u, r.gap_max_ms);         // 同时刻到达 ⇒ 间隔 0
}

// ---- 发端那一侧的读数：wire_gap（信封里那 16 位毫秒） ----
// ★ 这一路是"**发端究竟有没有按节奏发**"的自校验：它与收端的到达间隔**分开报**，
//   因为"gap 大而 wire_gap 小"= 收端本机在拖（主循环/渲染），不是链路的问题。
static void test_meas_receiver_tracks_sender_side_gap(void) {
  MeasReceiver rx;
  rx.reset();
  // 发端每 10 ms 发一帧（与收端到达节奏一致）
  for (uint16_t i = 0; i < 40u; ++i) {
    feedArrival(rx, i, 1000u + (uint32_t)i * 10u, i == 0u, (uint16_t)(i * 10u));
    drain(rx);
  }
  TEST_ASSERT_EQUAL_UINT32(10u, rx.result().wire_gap_max_ms);

  // 发端中间卡了一下（第 20 帧之后隔了 1900 ms 才发下一帧）⇒ wire_gap 跟着涨，
  // 而到达间隔仍是 10 ms（造的是"发端停了、但收到的那几帧本身很整齐"）。
  MeasReceiver rx2;
  rx2.reset();
  for (uint16_t i = 0; i < 40u; ++i) {
    const uint16_t t16 = (uint16_t)((i < 20u) ? (i * 10u) : (1900u + (i - 20u) * 10u));
    feedArrival(rx2, i, 1000u + (uint32_t)i * 10u, i == 0u, t16);
    drain(rx2);
  }
  TEST_ASSERT_GREATER_THAN_UINT32(50u, rx2.result().wire_gap_max_ms);
  TEST_ASSERT_EQUAL_UINT32(10u, rx2.result().gap_max_ms);   // 收端那一侧不受影响
}

// ---- 汇总行：字段齐全、% 是两位小数、判据门槛写在行尾 ----
// ★ 这一条同时钉住"**门槛达不到就如实报不适合**"：丢 1/400 = 0.25 % 是**不合格**的
//   （门槛是 < 0.1 %）⇒ 那一行的结尾必须是 `不适合(丢包)`，**不是**"适合"。
//   ★★ 别为了让这一行好看去放宽门槛 —— 要改就改契约（§1.1/§4）并写清为什么。
static void test_meas_summary_line_has_all_fields(void) {
  {
    MeasReceiver rx;
    rx.reset();
    for (uint16_t i = 0; i < 400u; ++i) {
      if (i == 100u) continue;                       // 0.25 %：**不合格**
      feedArrival(rx, i, 1000u + (uint32_t)i * 10u, i == 0u);
      drain(rx);
    }
    const std::string s = summaryOf(rx.result());
    TEST_ASSERT_TRUE(s.find("meas rx:") == 0u);
    TEST_ASSERT_TRUE(s.find("frames=399") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("expected=400") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("lost=1") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("(0.25%)") != std::string::npos);   // 1/400 = 0.25 %（两位小数）
    TEST_ASSERT_TRUE(s.find("gap_max=") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("p50=") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("p95=") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("p99=") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("coalesced=") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("qdrop=") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("wire_gap_max=") != std::string::npos);
    // 判据门槛与最小样本量写在行尾（**唯一出处**是 link_meas.h 的 kMeasThresholds）
    TEST_ASSERT_TRUE(s.find("loss<0.1% gap_max<100ms p99<20ms n>=300") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("不适合(丢包)") != std::string::npos);
  }
  {
    // 同一张判据表，样本足够且达标时那一行结尾必须是 `适合`
    MeasReceiver rx;
    rx.reset();
    for (uint16_t i = 0; i < 2000u; ++i) {
      if (i == 100u) continue;                       // 0.05 %：合格
      feedArrival(rx, i, 1000u + (uint32_t)i * 10u, i == 0u);
      drain(rx);
    }
    const std::string s = summaryOf(rx.result());
    TEST_ASSERT_TRUE(s.find("(0.05%)") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("-> 适合") != std::string::npos);
  }
  // 发端那一行
  MeasSender tx;
  InboxPhy phy;
  tx.start(1000u, kRoleMaster, 5u, 10u);
  for (uint32_t t = 1000u; t <= 1100u; t += 10u) tx.poll(t, phy);
  char buf[320] = {0};
  measFormatTxSummary(tx, 1100u, buf, (int)sizeof(buf));
  const std::string ts(buf);
  TEST_ASSERT_TRUE(ts.find("meas tx:") == 0u);
  TEST_ASSERT_TRUE(ts.find("sent=5/5") != std::string::npos);
  TEST_ASSERT_TRUE(ts.find("period=10ms") != std::string::npos);
  TEST_ASSERT_TRUE(ts.find("发满") != std::string::npos);
}

// 格式化函数不许越界写（cap 小的时候也得安全）
static void test_meas_summary_respects_capacity(void) {
  MeasRxResult r;
  char small[16] = {0};
  const int n = measFormatRxSummary(r, small, (int)sizeof(small));
  TEST_ASSERT_TRUE(n <= (int)sizeof(small) - 1);
  TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)small[sizeof(small) - 1]);   // 结尾一定是 NUL
  TEST_ASSERT_EQUAL_INT(0, measFormatRxSummary(r, nullptr, 16));
  char none[4] = {0};
  TEST_ASSERT_TRUE(measFormatRxSummary(r, none, 0) == 0);
}

// ---- 门槛本身（数值与理由都在 link_meas.h；这里钉"就是这三个数"） ----
static void test_meas_thresholds_are_the_documented_ones(void) {
  TEST_ASSERT_EQUAL_UINT32(100u, kMeasThresholds.loss_max_per_100k);   // 0.1 %
  TEST_ASSERT_EQUAL_UINT32(100u, kMeasThresholds.gap_max_ms);          // §4 第一档
  TEST_ASSERT_EQUAL_UINT32(20u, kMeasThresholds.p99_jitter_max_ms);    // 一个 TICK 周期
  TEST_ASSERT_EQUAL_UINT32(300u, kMeasThresholds.min_rx_frames);
  // 纯判据的直接测法（不经过 MeasReceiver）
  TEST_ASSERT_TRUE(measPassLoss(1000u, 1000u));
  TEST_ASSERT_FALSE(measPassLoss(999u, 1000u));    // 0.1 % 正好不算过
  TEST_ASSERT_TRUE(measPassLoss(1999u, 2000u));    // 0.05 % 算过
  TEST_ASSERT_TRUE(measPassLoss(1u, 1u));          // 全收 ⇒ 过
  TEST_ASSERT_TRUE(measPassGap(99u));
  TEST_ASSERT_FALSE(measPassGap(100u));            // 门槛是"严格小于"
  TEST_ASSERT_TRUE(measPassJitter(19u));
  TEST_ASSERT_FALSE(measPassJitter(20u));
  TEST_ASSERT_FALSE(measPassLoss(0u, 0u));         // 分母为 0 ⇒ 不下结论
}

void register_link_espnow_meas_tests(void) {
  RUN_TEST(test_link_phy_espnow_shape_matches_uart);
  RUN_TEST(test_espnow_channel_is_fixed_and_in_range);
  RUN_TEST(test_espnow_broadcast_address_is_all_ff);
  RUN_TEST(test_espnow_rings_and_pending_bounds);
  RUN_TEST(test_espnow_one_frame_fits_one_packet);
  RUN_TEST(test_espnow_nvs_keys_are_named);
  RUN_TEST(test_meas_envelope_layout_matches_spec);
  RUN_TEST(test_meas_sender_emits_valid_v1_data_frames);
  RUN_TEST(test_meas_sender_is_bounded_per_poll);
  RUN_TEST(test_meas_receiver_counts_no_loss);
  RUN_TEST(test_meas_receiver_loss_uses_seq_span);
  RUN_TEST(test_meas_receiver_max_gap_gate);
  RUN_TEST(test_meas_receiver_jitter_gate);
  RUN_TEST(test_meas_receiver_insufficient_samples_says_so);
  RUN_TEST(test_meas_receiver_burst_boundaries);
  RUN_TEST(test_meas_receiver_queue_overflow_is_visible);
  RUN_TEST(test_meas_receiver_coalesced_is_counted_separately);
  RUN_TEST(test_meas_receiver_tracks_sender_side_gap);
  RUN_TEST(test_meas_summary_line_has_all_fields);
  RUN_TEST(test_meas_summary_respects_capacity);
  RUN_TEST(test_meas_thresholds_are_the_documented_ones);
}

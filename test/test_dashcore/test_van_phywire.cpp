// van_phy_wire 测试:GPIO 边沿 → 解码 → VanPacket 整链
//
// 黄金向量来自 morcibacsi/VanAnalyzer 公开的真实抓包(readme 里手工解出的帧):
//   08.723351583333333 Ch:0: 8C4 C 8A 22 5A 81 5E A
//   08.736818458333332 Ch:0: 8D4 C 12 01 E8 2E A
//   08.799568666666666 Ch:0: 8C4 C 8A 21 40 3D 54 A
//   08.691351125000001 Ch:0: 5E4 C 20 1F 95 6A N
//   08.800854083333334 Ch:0: 4D4 E 85 0C 01 01 31 0E ...
// 格式 = 时间戳 / IDEN / CMD / DATA... / FCS / A|N
//
// 这些帧的 DATA 与 FCS 分界未公开(见 test_van_wire.cpp 的 FCS 说明),
// 所以这里用它们验证**我们能验证的部分**:IDEN/CMD 的打包能穿过整链不丢。
// 数据与 FCS 用我们自己的编码器生成,保证 CRC 自洽。
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "van_phy_wire.h"
#include "van_wire.h"
#include "van_source.h"

using namespace van;

namespace {

// 既记录又转发给 VanSource(验证数据层也认这个包)
class CaptureSink : public VanSink {
 public:
  explicit CaptureSink(VanSource* src) : src_(src) {}
  VanPacket last{};
  int count = 0;
  void onPacket(const VanPacket& p) override {
    last = p;
    ++count;
    if (src_) src_->onPacket(p);
  }

 private:
  VanSource* src_ = nullptr;
};

// 用编码器造一帧,把槽序列当边沿喂给 VanPhyWire。1 TS = 8µs(125 kbit/s)。
//
// 时基约定(踩了多次的坑,别再动):
//   空闲沿放 base_us,帧首沿放在 base_us + 8000µs(= 1000 个槽)。
//   - 空闲段必须是 10 的整数倍槽:4B5B 每 10 槽 = 1 字节,否则字节相位
//     残留半个字节,整帧错位(实测解出 0x70 而不是 0x0F)。
//   - 帧首沿不能与空闲沿同刻:Δt=0 会被钳成 1 个槽,吃掉帧的第一槽。
//
// 帧后**必须按槽展开一段 recessive**(tail_slots),不能只给一个边沿:
// 解码器按"沿之间有多少个槽"推进,而 EOF 判据要数到 8 个连续 recessive 槽;
// 若尾部只用一个长边沿表示,解码器看不到那些槽,帧永远不会收尾
// (实测:单帧测试 frames=0,帧被憋在解析器里)。
bool feedFrame(VanPhyWire& phy, const Frame& f, uint32_t base_us) {
  uint8_t slots[512];
  const uint32_t n = encodeFrame(f, slots, sizeof(slots));
  if (n == 0) return false;

  const uint32_t idle_slots = 1000u;               // 帧前空闲 = 1000 槽(10 的倍数)
  const uint32_t tail_slots = 100u;                // 帧后空闲 ≥ 8 槽即可,留足余量
  const uint32_t origin_slot = idle_slots;         // 帧首槽号
  bool level = true;                               // 总线空闲 = recessive
  phy.onEdge(base_us, level);                      // 空闲沿

  // 帧体:只在电平变化时才会有沿
  for (uint32_t i = 0; i < n; ++i) {
    const bool sl = slots[i] != 0;
    if (sl == level) continue;
    level = sl;
    phy.onEdge(base_us + (origin_slot + i) * 8u, level);
  }
  // 帧后空闲:按槽展开,保证解码器能数到 8 个连续 recessive
  for (uint32_t i = 0; i < tail_slots; ++i) {
    if (level) continue;                           // 已经是 recessive 就不需要沿
    level = true;
    phy.onEdge(base_us + (origin_slot + n + i) * 8u, level);
  }
  // 收尾:拉成 dominant,把最后一段区间结算出来
  const uint32_t end_slot = origin_slot + n + tail_slots;
  phy.onEdge(base_us + end_slot * 8u, false);
  return true;
}

}  // namespace

// 黄金向量:公开抓包里的 IDEN/CMD 组合走完整整链后必须一致
static void test_golden_iden_cmd_through_chain(void) {
  struct Golden { uint16_t iden; uint8_t cmd; };
  static const Golden g[] = {
      {0x8C4, 0xC},   // 8C4 C 8A 22 5A 81 5E A
      {0x8D4, 0xC},   // 8D4 C 12 01 E8 2E A
      {0x5E4, 0xC},   // 5E4 C 20 1F 95 6A N
      {0x4D4, 0xE},   // 4D4 E ... (CMD 各位都不同)
  };

  for (unsigned k = 0; k < sizeof(g) / sizeof(g[0]); ++k) {
    Frame f;
    f.ident = g[k].iden;
    f.cmd = g[k].cmd;
    f.len = 3;
    f.data[0] = 0x8A; f.data[1] = 0x22; f.data[2] = 0x5A;

    VanPhyWire phy;
    VanSource src;
    CaptureSink sink(&src);
    phy.begin();
    phy.setSink(&sink);

    TEST_ASSERT_TRUE(feedFrame(phy, f, 1000));

    char msg[96];
    snprintf(msg, sizeof(msg), "golden[%u] iden=0x%03X cmd=0x%X count=%d",
             k, g[k].iden, g[k].cmd, sink.count);
    TEST_ASSERT_TRUE_MESSAGE(sink.count == 1, msg);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(g[k].iden, sink.last.iden, msg);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(g[k].cmd, sink.last.cmd, msg);
    TEST_ASSERT_EQUAL_UINT8(3, sink.last.len);
    TEST_ASSERT_EQUAL_UINT8(0x8A, sink.last.data[0]);
    TEST_ASSERT_EQUAL_UINT8(1, sink.last.fcs_ok);   // 自洽帧的 CRC 必须通过
    TEST_ASSERT_EQUAL_INT(0, (int)phy.stats().frames_dropped);
    // 一帧物理帧只应计入一次统计(收尾可能在"凑出完整帧"与"总线空闲"
    // 两个时机各发生一次,第二次缓冲已清空 → 不计)
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)phy.stats().frames, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)phy.stats().frames_fcs_ok, msg);
  }
}

// 整链不得丢字节:同一帧连喂 3 次,必须解出 3 帧且内容一致
static void test_chain_consecutive_frames(void) {
  Frame f;
  f.ident = 0x824; f.cmd = 0xC; f.len = 2;
  f.data[0] = 0x11; f.data[1] = 0x22;

  VanPhyWire phy;
  VanSource src;
  CaptureSink sink(&src);
  phy.begin();
  phy.setSink(&sink);

  uint32_t t = 1000;
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(feedFrame(phy, f, t));
    t += 200000;                      // 帧间隔 200ms(远大于一帧)
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, sink.count, "连续 3 帧没有全部解出");
  TEST_ASSERT_EQUAL_HEX16(0x824, sink.last.iden);
  TEST_ASSERT_EQUAL_UINT8(2, sink.last.len);
  TEST_ASSERT_EQUAL_HEX8(0x11, sink.last.data[0]);
  TEST_ASSERT_EQUAL_HEX8(0x22, sink.last.data[1]);
  TEST_ASSERT_EQUAL_INT(3, (int)phy.stats().frames_fcs_ok);
}

// IDEN 的字节布局只承载 12 位:整帧走完一圈后,低 12 位必须一字不差。
// (高 3 位 bit12..14 在"整字节读取"的布局里没有位置 —— 见 van_wire.h 的
//  idenByte1/idenByte2 说明。规范里 IDEN 是 15 TS,但公开抓包的读法只取
//  12 位;这层换算要等实车原始位流确认,所以这里只钉 12 位的往返。)
static void test_chain_preserves_iden_bits(void) {
  Frame f;
  f.ident = 0x824;
  f.cmd = 0xC;
  f.len = 1;
  f.data[0] = 0x55;

  VanPhyWire phy;
  VanSource src;
  CaptureSink sink(&src);
  phy.begin();
  phy.setSink(&sink);
  TEST_ASSERT_TRUE(feedFrame(phy, f, 1000));

  TEST_ASSERT_EQUAL_INT(1, sink.count);
  TEST_ASSERT_EQUAL_HEX16(0x0824, (uint16_t)(sink.last.iden & 0x0FFFu));
  TEST_ASSERT_EQUAL_UINT8(0x0C, sink.last.cmd);
  TEST_ASSERT_EQUAL_UINT8(1, sink.last.fcs_ok);
}

// 数据层仍然认这个包:走 VanSource 之后车速/转速要出来
static void test_chain_feeds_van_source(void) {
  Frame f;
  f.ident = 0x824; f.cmd = 0xC; f.len = 7;
  // 18 F8 → 799rpm;车速 2710 = 10000 → 100.00 km/h(定标 0.01)
  const uint8_t d[7] = {0x18, 0xF8, 0x27, 0x10, 0x00, 0x00, 0x00};
  memcpy(f.data, d, 7);

  VanPhyWire phy;
  VanSource src;
  CaptureSink sink(&src);
  phy.begin();
  phy.setSink(&sink);
  TEST_ASSERT_TRUE(feedFrame(phy, f, 1000));

  TEST_ASSERT_EQUAL_INT(1, sink.count);
  TEST_ASSERT_TRUE(src.hasSpeed());
  TEST_ASSERT_TRUE(src.hasRpm());
  TEST_ASSERT_EQUAL_FLOAT(100.0f, src.speedKmh());
  TEST_ASSERT_EQUAL_FLOAT(799.0f, src.rpm());
}

// frameToPacket 的容量截断:超过 VanPacket 容量的数据按上限截断而不是越界
static void test_frame_to_packet_truncates(void) {
  Frame f;
  f.ident = 0x824; f.cmd = 0xC;
  f.len = kDataDefault;
  for (uint8_t i = 0; i < kDataDefault; ++i) f.data[i] = (uint8_t)(i + 1);
  f.fcs_ok = true;
  f.ack = 1;

  VanPacket p{};
  frameToPacket(f, 1234, &p);
  TEST_ASSERT_EQUAL_UINT16(0x824, p.iden);
  TEST_ASSERT_EQUAL_UINT8(0xC, p.cmd);
  TEST_ASSERT_EQUAL_UINT8(1, p.ack);
  TEST_ASSERT_EQUAL_UINT8(1, p.fcs_ok);
  TEST_ASSERT_EQUAL_UINT32(1234, p.rx_ms);
  TEST_ASSERT_TRUE(p.len <= sizeof(p.data));
  for (uint8_t i = 0; i < p.len; ++i) TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1), p.data[i]);
}

void register_van_phy_wire_tests(void) {
  RUN_TEST(test_golden_iden_cmd_through_chain);
  RUN_TEST(test_chain_consecutive_frames);
  RUN_TEST(test_chain_preserves_iden_bits);
  RUN_TEST(test_chain_feeds_van_source);
  RUN_TEST(test_frame_to_packet_truncates);
}

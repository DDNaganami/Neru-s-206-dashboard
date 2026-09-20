#include <unity.h>
#include "van_source.h"

static VanPacket makePacket(uint16_t iden, uint8_t len, uint32_t rx_ms) {
  VanPacket p{};
  p.iden = iden;
  p.len = len;
  p.rx_ms = rx_ms;
  return p;
}

void test_van_speed_and_rpm(void) {
  VanSource van;
  VanPacket p = makePacket(0x824, 7, 1000);
  p.data[0] = 0x18; p.data[1] = 0xF8;  // 6392 → 799 rpm
  p.data[2] = 0x64;                    // 100 → 100 km/h(★ 车速是**单字节**,1 计数 = 1 km/h)
  van.onPacket(p);

  TEST_ASSERT_TRUE(van.hasSpeed());
  TEST_ASSERT_EQUAL_FLOAT(100.0f, van.speedKmh());
  TEST_ASSERT_TRUE(van.hasRpm());
  TEST_ASSERT_EQUAL_FLOAT(799.0f, van.rpm());
  TEST_ASSERT_EQUAL_UINT32(1000, van.lastUpdateMs());
}

// 车速字段是**单字节**:data[3] 必须完全不参与车速(它是另一个未知字段)。
// 这条钉住"有人把它改回 16 位读法"的回退 —— 16 位读法会把 data[3] 的影响带进来。
void test_van_speed_is_single_byte(void) {
  VanSource a, b;
  VanPacket pa = makePacket(0x824, 7, 1000);
  pa.data[0] = 0x1C; pa.data[1] = 0xA2; pa.data[2] = 0x0D; pa.data[3] = 0x4B;
  VanPacket pb = pa;
  pb.data[3] = 0xFF;                   // 只改 data[3]
  a.onPacket(pa);
  b.onPacket(pb);
  TEST_ASSERT_EQUAL_FLOAT(13.0f, a.speedKmh());
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(13.0f, b.speedKmh(),
                                  "data[3] 不该影响车速 —— 车速是单字节 data[2]");
}

void test_van_clamp_bad_speed(void) {
  VanSource van;
  VanPacket p = makePacket(0x824, 7, 2000);
  p.data[0] = 0x18; p.data[1] = 0xF8;  // 合法转速应照常通过
  p.data[2] = 0x64;                    // 100 km/h 合法
  van.onPacket(p);
  TEST_ASSERT_TRUE(van.hasSpeed());
  TEST_ASSERT_EQUAL_FLOAT(100.0f, van.speedKmh());

  // 值域钳制仍要挡得住坏帧:默认 scale=1.0 时 8 位字段最大 255 km/h(< 300),
  // 所以用 configureSpeedFrame 把标度放大到 2.0,让 0xFF(510 km/h)越界。
  VanSource van2;
  van2.configureSpeedFrame(VanSource::kSpeedIden, VanSource::kSpeedOffset, 2.0f);
  VanPacket q = makePacket(0x824, 7, 2100);
  q.data[0] = 0x18; q.data[1] = 0xF8;
  q.data[2] = 0xFF;                    // 255 × 2.0 = 510 > 300 → 丢弃
  van2.onPacket(q);
  TEST_ASSERT_FALSE(van2.hasSpeed());
  TEST_ASSERT_TRUE(van2.hasRpm());
  TEST_ASSERT_EQUAL_FLOAT(799.0f, van2.rpm());
}

void test_van_wrong_iden(void) {
  VanSource van;
  VanPacket p = makePacket(0x123, 7, 3000);
  p.data[2] = 0x64;
  van.onPacket(p);
  TEST_ASSERT_FALSE(van.hasSpeed());
  TEST_ASSERT_FALSE(van.hasRpm());
}

void test_van_configure_speed_frame(void) {
  VanSource van;
  van.configureSpeedFrame(0x864, 4, 1.0f);

  VanPacket p = makePacket(0x864, 6, 4000);
  p.data[4] = 0x7B;                    // 123 → 123 km/h(单字节,scale 1.0)
  van.onPacket(p);
  TEST_ASSERT_EQUAL_FLOAT(123.0f, van.speedKmh());
}

void register_van_source_tests(void) {
  RUN_TEST(test_van_speed_and_rpm);
  RUN_TEST(test_van_speed_is_single_byte);
  RUN_TEST(test_van_clamp_bad_speed);
  RUN_TEST(test_van_wrong_iden);
  RUN_TEST(test_van_configure_speed_frame);
}

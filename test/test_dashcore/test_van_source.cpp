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
  p.data[2] = 0x27; p.data[3] = 0x10;  // 10000 → 100.0 km/h
  van.onPacket(p);

  TEST_ASSERT_TRUE(van.hasSpeed());
  TEST_ASSERT_EQUAL_FLOAT(100.0f, van.speedKmh());
  TEST_ASSERT_TRUE(van.hasRpm());
  TEST_ASSERT_EQUAL_FLOAT(799.0f, van.rpm());
  TEST_ASSERT_EQUAL_UINT32(1000, van.lastUpdateMs());
}

void test_van_clamp_bad_speed(void) {
  VanSource van;
  VanPacket p = makePacket(0x824, 7, 2000);
  p.data[0] = 0x18; p.data[1] = 0xF8;  // 合法转速应照常通过
  p.data[2] = 0xFF; p.data[3] = 0xFF;  // 655.35 km/h > 300 → 丢弃
  van.onPacket(p);

  TEST_ASSERT_FALSE(van.hasSpeed());
  TEST_ASSERT_TRUE(van.hasRpm());
  TEST_ASSERT_EQUAL_FLOAT(799.0f, van.rpm());
}

void test_van_wrong_iden(void) {
  VanSource van;
  VanPacket p = makePacket(0x123, 7, 3000);
  p.data[2] = 0x27; p.data[3] = 0x10;
  van.onPacket(p);
  TEST_ASSERT_FALSE(van.hasSpeed());
  TEST_ASSERT_FALSE(van.hasRpm());
}

void test_van_configure_speed_frame(void) {
  VanSource van;
  van.configureSpeedFrame(0x864, 4, 1.0f);

  VanPacket p = makePacket(0x864, 6, 4000);
  p.data[4] = 0x00; p.data[5] = 0x7B;  // 123 km/h
  van.onPacket(p);
  TEST_ASSERT_EQUAL_FLOAT(123.0f, van.speedKmh());
}

void register_van_source_tests(void) {
  RUN_TEST(test_van_speed_and_rpm);
  RUN_TEST(test_van_clamp_bad_speed);
  RUN_TEST(test_van_wrong_iden);
  RUN_TEST(test_van_configure_speed_frame);
}

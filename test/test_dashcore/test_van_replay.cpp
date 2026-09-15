#include <unity.h>
#include "van_replay.h"
#include "data_service.h"

void test_parse_spaced(void) {
  VanPacket p{};
  TEST_ASSERT_TRUE(parseVanReplayLine("VAN 824 18 F8 27 10 00 00 00", &p, 1000));
  TEST_ASSERT_EQUAL_UINT16(0x824, p.iden);
  TEST_ASSERT_EQUAL_UINT8(7, p.len);
  TEST_ASSERT_EQUAL_UINT8(0x18, p.data[0]);
  TEST_ASSERT_EQUAL_UINT8(0xF8, p.data[1]);
  TEST_ASSERT_EQUAL_UINT8(0x27, p.data[2]);
  TEST_ASSERT_EQUAL_UINT8(0x10, p.data[3]);
  TEST_ASSERT_EQUAL_UINT32(1000, p.rx_ms);
}

void test_parse_compact_lowercase(void) {
  VanPacket p{};
  TEST_ASSERT_TRUE(parseVanReplayLine("van82418f82710000000", &p, 2000));
  TEST_ASSERT_EQUAL_UINT16(0x824, p.iden);
  TEST_ASSERT_EQUAL_UINT8(7, p.len);
}

void test_parse_padded_iden(void) {
  VanPacket p{};
  TEST_ASSERT_TRUE(parseVanReplayLine("VAN 07C FF", &p, 0));
  TEST_ASSERT_EQUAL_UINT16(0x07C, p.iden);
  TEST_ASSERT_EQUAL_UINT8(1, p.len);
  TEST_ASSERT_EQUAL_UINT8(0xFF, p.data[0]);
}

void test_parse_garbage(void) {
  VanPacket p{};
  TEST_ASSERT_FALSE(parseVanReplayLine("CAN 824 18F8", &p, 0));   // 不是 VAN
  TEST_ASSERT_FALSE(parseVanReplayLine("VAN 82 18F8", &p, 0));    // iden 不足 3 位
  TEST_ASSERT_FALSE(parseVanReplayLine("VAN 824 1", &p, 0));      // 奇数位
  TEST_ASSERT_FALSE(parseVanReplayLine("VAN 824 G1", &p, 0));     // 非十六进制
  TEST_ASSERT_FALSE(parseVanReplayLine("", &p, 0));
  TEST_ASSERT_FALSE(parseVanReplayLine(nullptr, &p, 0));
}

// 端到端:回放帧 → onVanPacket → data_service 合并 → 车速走 Van
void test_replay_feeds_data_service(void) {
  VehicleDataService svc(nullptr);
  svc.begin();

  VanPacket p{};
  TEST_ASSERT_TRUE(parseVanReplayLine("VAN 824 18F82710000000", &p, 1000));
  svc.onVanPacket(p);

  const VehicleState st = svc.update(1000);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Van);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, st.speed_kmh);
  TEST_ASSERT_EQUAL_FLOAT(799.0f, st.rpm);
}

void register_van_replay_tests(void) {
  RUN_TEST(test_parse_spaced);
  RUN_TEST(test_parse_compact_lowercase);
  RUN_TEST(test_parse_padded_iden);
  RUN_TEST(test_parse_garbage);
  RUN_TEST(test_replay_feeds_data_service);
}

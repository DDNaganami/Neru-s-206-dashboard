#include <unity.h>
#include "van_replay.h"
#include "data_service.h"

// 回放行语法:"VAN <iden> [cmd] <data...>"
//   iden 3~4 位十六进制;cmd 为可选的单个十六进制位(孤立 token)

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
  TEST_ASSERT_EQUAL_UINT8(0x8, p.cmd);      // 省略 CMD 时按 EXT=1
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

// 15 位 IDEN:4 位十六进制,能表达 0x000/0xFFF 这类保留值
void test_parse_15bit_iden(void) {
  VanPacket p{};
  TEST_ASSERT_TRUE(parseVanReplayLine("VAN 1824 11 22", &p, 0));
  TEST_ASSERT_EQUAL_UINT16(0x1824, p.iden);   // bit12 置位
  TEST_ASSERT_EQUAL_UINT8(2, p.len);

  VanPacket q{};
  TEST_ASSERT_TRUE(parseVanReplayLine("VAN 0FFF AA", &q, 0));
  TEST_ASSERT_EQUAL_UINT16(0x0FFF, q.iden);   // 规范里的保留值要能表达
}

// 显式 CMD:孤立的一个十六进制位
void test_parse_explicit_cmd(void) {
  VanPacket p{};
  // 对应公开抓包格式 "VAN <iden> <cmd> <data...>"
  TEST_ASSERT_TRUE(parseVanReplayLine("VAN 8C4 C 8A 22 5A", &p, 0));
  TEST_ASSERT_EQUAL_UINT16(0x8C4, p.iden);
  TEST_ASSERT_EQUAL_UINT8(0xC, p.cmd);
  TEST_ASSERT_EQUAL_UINT8(3, p.len);
  TEST_ASSERT_EQUAL_UINT8(0x8A, p.data[0]);
  TEST_ASSERT_EQUAL_UINT8(0x22, p.data[1]);
  TEST_ASSERT_EQUAL_UINT8(0x5A, p.data[2]);

  // 只有一个孤立位时不带数据:CMD 被正确吃掉,数据为空
  VanPacket q{};
  TEST_ASSERT_TRUE(parseVanReplayLine("VAN 824 1", &q, 0));
  TEST_ASSERT_EQUAL_UINT8(1, q.cmd);
  TEST_ASSERT_EQUAL_UINT8(0, q.len);
}

void test_parse_garbage(void) {
  VanPacket p{};
  TEST_ASSERT_FALSE(parseVanReplayLine("CAN 824 18F8", &p, 0));   // 不是 VAN
  TEST_ASSERT_FALSE(parseVanReplayLine("VAN 82 18F8", &p, 0));    // iden 不足 3 位
  TEST_ASSERT_FALSE(parseVanReplayLine("VAN 824 G1", &p, 0));     // 非十六进制
  TEST_ASSERT_FALSE(parseVanReplayLine("VAN 824 18 F", &p, 0));   // 奇数位
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
  RUN_TEST(test_parse_15bit_iden);
  RUN_TEST(test_parse_explicit_cmd);
  RUN_TEST(test_parse_garbage);
  RUN_TEST(test_replay_feeds_data_service);
}

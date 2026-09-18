#include <unity.h>
#include "obd_protocol.h"

static void expect_parse(const char* line, uint8_t pid, uint16_t raw) {
  uint8_t p = 0;
  uint16_t r = 0;
  TEST_ASSERT_TRUE(parseObdLine(line, &p, &r));
  TEST_ASSERT_EQUAL_UINT8(pid, p);
  TEST_ASSERT_EQUAL_UINT16(raw, r);
}

void test_010c_two_bytes_spaced(void) { expect_parse("41 0C 1A F8", 0x0C, 0x1AF8); }
void test_010c_compact(void) { expect_parse("410C1AF8", 0x0C, 0x1AF8); }
void test_010c_with_header(void) { expect_parse("8F 41 0C 1A F8", 0x0C, 0x1AF8); }

// 回归:旧实现要求 0105 响应有两个字节,标准单字节水温永远解析不出来
void test_0105_single_byte(void) { expect_parse("41 05 3C", 0x05, 0x3C); }
void test_0105_with_header(void) { expect_parse("8F 41 05 5A", 0x05, 0x5A); }

// 010F 进气温度:与 0105 同形(单字节 A-40)。
// 2026-09 用户用蓝牙 ELM327 实测这台车支持这一项,所以它进了轮询表。
// 解析层本来就不区分 PID,这条测的是"确实能走到换算函数"。
void test_010f_intake_single_byte(void) { expect_parse("41 0F 2A", 0x0F, 0x2A); }
void test_010f_with_header(void) { expect_parse("8F 41 0F 3C", 0x0F, 0x3C); }

void test_garbage_lines(void) {
  uint8_t p;
  uint16_t r;
  TEST_ASSERT_FALSE(parseObdLine("SEARCHING...", &p, &r));
  // ★ "NO DATA" 是 ECU 不支持某个 PID 时的标准回复 —— 这台车如果哪一版 ECU
  //   不认 010F,表现就是这一行,解析失败 → hasIntake() 一直是 false →
  //   上层按字段回退到假数据。**不能**把它解析成 0。
  TEST_ASSERT_FALSE(parseObdLine("NO DATA", &p, &r));
  TEST_ASSERT_FALSE(parseObdLine("", &p, &r));
  TEST_ASSERT_FALSE(parseObdLine(nullptr, &p, &r));
}

void test_converters(void) {
  TEST_ASSERT_EQUAL_FLOAT(1726.0f, rpmFromRaw(0x1AF8));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, coolantFromRaw(0x3C));
  TEST_ASSERT_EQUAL_FLOAT(-40.0f, coolantFromRaw(0x00));
  // 进气温度:与水温同一套换算(A-40),但函数名分开 —— 将来某个 ECU
  // 换成两字节时改这里不会误伤冷却液。
  TEST_ASSERT_EQUAL_FLOAT(2.0f, intakeFromRaw(0x2A));    // 42-40
  TEST_ASSERT_EQUAL_FLOAT(20.0f, intakeFromRaw(0x3C));   // 60-40
  TEST_ASSERT_EQUAL_FLOAT(-40.0f, intakeFromRaw(0x00));
  TEST_ASSERT_EQUAL_FLOAT(coolantFromRaw(0x2A), intakeFromRaw(0x2A));
}

void register_obd_protocol_tests(void) {
  RUN_TEST(test_010c_two_bytes_spaced);
  RUN_TEST(test_010c_compact);
  RUN_TEST(test_010c_with_header);
  RUN_TEST(test_0105_single_byte);
  RUN_TEST(test_0105_with_header);
  RUN_TEST(test_010f_intake_single_byte);
  RUN_TEST(test_010f_with_header);
  RUN_TEST(test_garbage_lines);
  RUN_TEST(test_converters);
}

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

void test_garbage_lines(void) {
  uint8_t p;
  uint16_t r;
  TEST_ASSERT_FALSE(parseObdLine("SEARCHING...", &p, &r));
  TEST_ASSERT_FALSE(parseObdLine("NO DATA", &p, &r));
  TEST_ASSERT_FALSE(parseObdLine("", &p, &r));
  TEST_ASSERT_FALSE(parseObdLine(nullptr, &p, &r));
}

void test_converters(void) {
  TEST_ASSERT_EQUAL_FLOAT(1726.0f, rpmFromRaw(0x1AF8));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, coolantFromRaw(0x3C));
  TEST_ASSERT_EQUAL_FLOAT(-40.0f, coolantFromRaw(0x00));
}

void register_obd_protocol_tests(void) {
  RUN_TEST(test_010c_two_bytes_spaced);
  RUN_TEST(test_010c_compact);
  RUN_TEST(test_010c_with_header);
  RUN_TEST(test_0105_single_byte);
  RUN_TEST(test_0105_with_header);
  RUN_TEST(test_garbage_lines);
  RUN_TEST(test_converters);
}

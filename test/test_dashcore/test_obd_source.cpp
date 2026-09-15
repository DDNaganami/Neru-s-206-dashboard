#include <unity.h>
#include "test_helpers.h"
#include "obd_source.h"

// 驱动状态机直到 fake 串口发出指定命令
static bool drive_until_tx(FakeSerial& fake, ObdSource& obd, uint32_t& t,
                           const char* cmd, int max_steps) {
  for (int i = 0; i < max_steps; ++i) {
    t += 50;
    obd.tick(t);
    if (fake.sent(cmd)) return true;
  }
  return false;
}

void test_obd_init_sequence(void) {
  FakeSerial fake;
  ObdSource obd(&fake);
  test_set_millis(0);
  obd.begin();
  TEST_ASSERT_TRUE(fake.sent("ATZ\r"));

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "ATE0\r", 30));
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "ATL0\r", 30));
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "ATH0\r", 30));
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 30));
  // 轮询:010C 之后轮到 0105
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0105\r", 30));
}

void test_obd_rpm_coolant_parse(void) {
  FakeSerial fake;
  ObdSource obd(&fake);
  test_set_millis(0);
  obd.begin();

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 40));

  fake.feed("41 0C 1A F8\r");
  t += 10;
  obd.tick(t);
  TEST_ASSERT_TRUE(obd.hasRpm());
  TEST_ASSERT_EQUAL_FLOAT(1726.0f, obd.rpm());
  TEST_ASSERT_EQUAL_UINT32(t, obd.lastRpmMs());
  TEST_ASSERT_FALSE(obd.hasCoolant());  // 水温还没喂过

  // 单字节水温响应
  fake.feed("41 05 3C\r");
  t += 10;
  obd.tick(t);
  TEST_ASSERT_TRUE(obd.hasCoolant());
  TEST_ASSERT_EQUAL_FLOAT(20.0f, obd.coolant());
  TEST_ASSERT_EQUAL_UINT32(t, obd.lastCoolantMs());
}

void test_obd_bad_rpm_rejected(void) {
  FakeSerial fake;
  ObdSource obd(&fake);
  test_set_millis(0);
  obd.begin();

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 40));

  fake.feed("41 0C 1A F8\r");  // 先来一帧合法值
  t += 10;
  obd.tick(t);
  TEST_ASSERT_EQUAL_FLOAT(1726.0f, obd.rpm());

  fake.feed("41 0C FF FF\r");  // 16383.75 rpm > 9000,应丢弃
  t += 10;
  obd.tick(t);
  TEST_ASSERT_EQUAL_FLOAT(1726.0f, obd.rpm());
}

void test_obd_disabled(void) {
  ObdSource obd(nullptr);
  obd.begin();     // 不崩
  obd.tick(5000);  // 不崩
  TEST_ASSERT_FALSE(obd.enabled());
}

void register_obd_source_tests(void) {
  RUN_TEST(test_obd_init_sequence);
  RUN_TEST(test_obd_rpm_coolant_parse);
  RUN_TEST(test_obd_bad_rpm_rejected);
  RUN_TEST(test_obd_disabled);
}

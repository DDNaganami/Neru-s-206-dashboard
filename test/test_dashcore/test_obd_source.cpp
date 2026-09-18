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
  // 轮询:010C → 0105 → 010F(进气温度)→ 回到 010C
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "0105\r", 30));
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010F\r", 30));
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010C\r", 30));
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

// 进气温度(010F):与水温同形,单独喂一帧看它进没进状态。
void test_obd_intake_parse(void) {
  FakeSerial fake;
  ObdSource obd(&fake);
  test_set_millis(0);
  obd.begin();

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010F\r", 60));
  TEST_ASSERT_FALSE(obd.hasIntake());          // 还没喂数据

  fake.feed("41 0F 2A\r");                     // 0x2A = 42 → 2℃
  t += 10;
  obd.tick(t);
  TEST_ASSERT_TRUE(obd.hasIntake());
  TEST_ASSERT_EQUAL_FLOAT(2.0f, obd.intake());
  TEST_ASSERT_EQUAL_UINT32(t, obd.lastIntakeMs());
  // 三路互不干扰:喂了进气温度不等于喂了水温/转速
  TEST_ASSERT_FALSE(obd.hasCoolant());
  TEST_ASSERT_FALSE(obd.hasRpm());
}

// ★ ECU 不支持 010F 时的真实形态:ELM327 回 "NO DATA"。
//   这条必须**什么都不改**(hasIntake 保持 false),否则会出现"进气 0℃"的假读数。
void test_obd_intake_no_data_keeps_invalid(void) {
  FakeSerial fake;
  ObdSource obd(&fake);
  test_set_millis(0);
  obd.begin();

  uint32_t t = 1000;
  TEST_ASSERT_TRUE(drive_until_tx(fake, obd, t, "010F\r", 60));
  fake.feed("NO DATA\r");
  t += 10;
  obd.tick(t);
  TEST_ASSERT_FALSE(obd.hasIntake());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, obd.intake());
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
  RUN_TEST(test_obd_intake_parse);
  RUN_TEST(test_obd_intake_no_data_keeps_invalid);
  RUN_TEST(test_obd_bad_rpm_rejected);
  RUN_TEST(test_obd_disabled);
}

#include <unity.h>
#include "test_helpers.h"
#include "data_service.h"

// 以 50ms 步长推进虚拟时间,反复跑 update
static void advance(VehicleDataService& svc, uint32_t& t, uint32_t ms) {
  const uint32_t end = t + ms;
  while (t < end) {
    t += 50;
    svc.update(t);
  }
}

void test_sim_only(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  const VehicleState st = svc.update(1000);
  TEST_ASSERT_TRUE(st.speed_kmh >= 0.0f && st.speed_kmh <= kSpeedMax);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Sim);
}

void test_obd_rpm_takeover_per_field_and_fallback(void) {
  FakeSerial fake;
  VehicleDataService svc(&fake);
  test_set_millis(0);
  svc.begin();

  uint32_t t = 1000;
  svc.update(t);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Sim);

  // 推进状态机直到发出 010C
  for (int i = 0; i < 60 && !fake.sent("010C\r"); ++i) {
    t += 50;
    svc.update(t);
  }
  TEST_ASSERT_TRUE(fake.sent("010C\r"));

  fake.feed("41 0C 1A F8\r");  // 1726 rpm
  t += 10;
  const VehicleState st = svc.update(t);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Obd);
  TEST_ASSERT_EQUAL_FLOAT(1726.0f, st.rpm);
  // 水温从没喂过 → 按字段独立超时,仍走假数据(旧实现共用时间戳会误判为 OBD)
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Sim);

  // 断线 3.5 秒 → 转速回退 Sim
  advance(svc, t, 3500);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Sim);
}

void test_van_speed_and_fallback(void) {
  VehicleDataService svc(nullptr);
  svc.begin();
  uint32_t t = 1000;

  VanPacket p{};
  p.iden = VanSource::kSpeedIden;
  p.len = 7;
  p.data[0] = 0x18; p.data[1] = 0xF8;  // 799 rpm
  p.data[2] = 0x27; p.data[3] = 0x10;  // 100.0 km/h
  p.rx_ms = t;
  svc.onVanPacket(p);

  const VehicleState st = svc.update(t);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Van);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Van);  // 无 OBD 时 VAN 转速兜底
  TEST_ASSERT_EQUAL_FLOAT(100.0f, st.speed_kmh);

  advance(svc, t, 3500);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Sim);
}

void test_obd_rpm_beats_van(void) {
  FakeSerial fake;
  VehicleDataService svc(&fake);
  test_set_millis(0);
  svc.begin();

  uint32_t t = 1000;
  for (int i = 0; i < 60 && !fake.sent("010C\r"); ++i) {
    t += 50;
    svc.update(t);
  }
  fake.feed("41 0C 1A F8\r");
  t += 10;
  svc.update(t);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Obd);

  VanPacket p{};
  p.iden = VanSource::kSpeedIden;
  p.len = 7;
  p.data[0] = 0x10; p.data[1] = 0x00;  // 512 rpm
  p.data[2] = 0x13; p.data[3] = 0x88;  // 50 km/h
  p.rx_ms = t;
  svc.onVanPacket(p);
  svc.update(t);

  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Van);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Obd);  // OBD 转速优先于 VAN
}

void register_data_service_tests(void) {
  RUN_TEST(test_sim_only);
  RUN_TEST(test_obd_rpm_takeover_per_field_and_fallback);
  RUN_TEST(test_van_speed_and_fallback);
  RUN_TEST(test_obd_rpm_beats_van);
}

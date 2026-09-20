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
  // 进气温度也只有假数据源(206 的 VAN 上没有这一项)
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Sim);
}

// 进气温度(010F):OBD 接管 → 独立超时回退,且**不影响转速/水温**。
// 它是速度表的副表,这条同时钉住"加水温表时踩过的那个坑":
// 三路必须各记各的时间戳(旧实现共用时间戳 → 一路有数据就把另外两路也判成 OBD)。
void test_obd_intake_takeover_per_field(void) {
  FakeSerial fake;
  VehicleDataService svc(&fake);
  test_set_millis(0);
  svc.begin();

  uint32_t t = 1000;
  svc.update(t);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Sim);

  // 推进状态机直到发出 010F(轮询表第 3 格,所以要等两轮)
  for (int i = 0; i < 90 && !fake.sent("010F\r"); ++i) {
    t += 50;
    svc.update(t);
  }
  TEST_ASSERT_TRUE(fake.sent("010F\r"));

  fake.feed("41 0F 3C\r");     // 0x3C = 60 → 20℃
  t += 10;
  const VehicleState st = svc.update(t);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Obd);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, st.intake_c);
  // 只喂了进气温度:转速/水温仍走假数据 —— 这就是"按字段独立"
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Sim);
  TEST_ASSERT_TRUE(svc.status().coolant == FieldSource::Sim);

  // 断线 3.5 秒 → 进气温度也回退 Sim(不会有"停在 20℃ 不动"的僵尸值)
  advance(svc, t, 3500);
  TEST_ASSERT_TRUE(svc.status().intake == FieldSource::Sim);
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
  p.data[2] = 0x64;                    // 100 km/h(★ 单字节,1 计数 = 1 km/h)
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
  p.data[2] = 0x32;                    // 50 km/h
  p.rx_ms = t;
  svc.onVanPacket(p);
  svc.update(t);

  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Van);
  TEST_ASSERT_TRUE(svc.status().rpm == FieldSource::Obd);  // OBD 转速优先于 VAN
}

// ★ 车速的优先级:Van > Obd > Sim。
//   为什么这三档都要有:
//     · VAN 上的车速是仪表**广播**的,零 K 线成本 → 优先,把时隙留给转速;
//     · OBD 的 010D 是兜底 —— VAN 没接/没解出帧时车速照样是真值(≈1Hz),
//       而不是掉回假数据;
//     · 两个都断(3 秒)才回假数据。
//   这条同时钉住"两段代码的先后顺序就是优先级"这个实现方式。
void test_speed_priority_van_obd_sim(void) {
  FakeSerial fake;
  VehicleDataService svc(&fake);
  test_set_millis(0);
  svc.begin();

  uint32_t t = 1000;
  svc.update(t);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Sim);

  // 1) 位图说支持 010D → 表里就有它;喂一帧 60 km/h → 车速变成 Obd
  for (int i = 0; i < 60 && !fake.sent("0100\r"); ++i) { t += 50; svc.update(t); }
  fake.feed("41 00 BE 3E B8 13\r");
  for (int i = 0; i < 120 && !fake.sent("010D\r"); ++i) { t += 50; svc.update(t); }
  TEST_ASSERT_TRUE(fake.sent("010D\r"));
  fake.feed("41 0D 3C\r");                      // 60 km/h
  t += 10;
  VehicleState st = svc.update(t);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Obd);
  TEST_ASSERT_EQUAL_FLOAT(60.0f, st.speed_kmh);

  // 2) VAN 来一帧 100 km/h → 立刻压过 OBD(不是"等 OBD 过期")
  VanPacket p{};
  p.iden = VanSource::kSpeedIden;
  p.len = 7;
  p.data[0] = 0x18; p.data[1] = 0xF8;
  p.data[2] = 0x64;                            // 100 km/h(单字节)
  p.rx_ms = t;
  svc.onVanPacket(p);
  st = svc.update(t);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Van);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, st.speed_kmh);

  // 3) 两个都断 3.5 秒 → 回假数据(既不是停在 100,也不是停在 60)
  advance(svc, t, 3500);
  TEST_ASSERT_TRUE(svc.status().speed == FieldSource::Sim);
}

// OBD 那四个实测刷新率必须真的到得了上层日志(不然到车上就没法判断
// "K 线够不够用")。
void test_obd_rates_reach_status(void) {
  FakeSerial fake;
  VehicleDataService svc(&fake);
  test_set_millis(0);
  svc.begin();

  uint32_t t = 1000;
  // 先把 OBD 的刷新率窗口对齐(推进到一次结算之后 → 窗口起点确定、hz 归零)
  advance(svc, t, 1500);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, svc.status().obd_rpm_hz);
  fake.feed("41 0C 1A F8\r");
  fake.feed("41 0C 1B 00\r");
  t += 10;
  svc.update(t);
  // 跨过 1 秒的结算点
  advance(svc, t, 1000);
  TEST_ASSERT_TRUE(svc.status().obd_rpm_hz > 0.0f);
  TEST_ASSERT_TRUE(svc.status().obd_speed_hz == 0.0f);   // 一路都没问过车速
}

void register_data_service_tests(void) {
  RUN_TEST(test_sim_only);
  RUN_TEST(test_obd_intake_takeover_per_field);
  RUN_TEST(test_obd_rpm_takeover_per_field_and_fallback);
  RUN_TEST(test_van_speed_and_fallback);
  RUN_TEST(test_obd_rpm_beats_van);
  RUN_TEST(test_speed_priority_van_obd_sim);
  RUN_TEST(test_obd_rates_reach_status);
}

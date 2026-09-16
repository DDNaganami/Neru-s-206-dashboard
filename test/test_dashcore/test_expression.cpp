#include <unity.h>
#include "expression.h"

// 默认水温取"正常运行温度":VehicleState 的默认值是 20(冷车),而冷车现在
// 会走 Face::Cold —— 想测转速/速度那两路就必须显式把水温写成 85,
// 否则每条用例都会得到 Cold。这不是测试的将就,是产品行为:
//   水温 20 度时屏幕上就该是冷车脸。
static VehicleState mk(float speed, float rpm, float coolant = 85.0f) {
  VehicleState s;
  s.speed_kmh = speed;
  s.rpm = rpm;
  s.coolant_c = coolant;
  return s;
}

// 注意:face_update 是带静态状态的状态机,用例按顺序执行、速度平缓过渡
// (每步 Δv ≤ 15,即 ≤ 15 km/h/s,不触发惊喜),只验证基础表情与眨眼。
// 眨眼计时从第一帧算起(第一帧 + 3000ms),所以这里第一帧取 t=1000、
// 眨眼落在 t=4000 —— 这个偏移是刻意的,见 expression.cpp 里的说明。
void test_face_base_states(void) {
  face_reset();
  uint32_t t = 1000;
  TEST_ASSERT_TRUE(face_update(mk(0, 800), t) == Face::Idle);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(12, 1400), t) == Face::Idle);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(24, 2000), t) == Face::Idle);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(24, 2000), t) == Face::Blink);  // 第一帧后 3 秒眨一次
  t += 200;
  TEST_ASSERT_TRUE(face_update(mk(24, 2000), t) == Face::Idle);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(36, 2600), t) == Face::Cruise);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(48, 3200), t) == Face::Cruise);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(60, 3800), t) == Face::Cruise);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(72, 4200), t) == Face::Cruise);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(84, 4400), t) == Face::Cruise);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(90, 4600), t) == Face::Sport);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(90, 6500), t) == Face::Redline);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(10, 900), t) == Face::Blink);  // 回到怠速再眨一次
  t += 200;
  TEST_ASSERT_TRUE(face_update(mk(10, 900), t) == Face::Idle);
}

// 转速单独就能把表情推到巡航/运动(以前只看速度,怠速轰油门屏幕毫无反应)。
// 阈值:2500 巡航 / 4500 运动 / 6000 红区。
void test_rpm_alone_drives_face(void) {
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 2499), 1000) == Face::Idle);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 2500), 1000) == Face::Cruise);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 4499), 1000) == Face::Cruise);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 4500), 1000) == Face::Sport);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 5999), 1000) == Face::Sport);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 6000), 1000) == Face::Redline);
}

// 水温两档:低于 70 冷车、高于等于 105 过热;中间不改变表情。
void test_coolant_drives_face(void) {
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 900, 20.0f), 1000) == Face::Cold);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 900, 69.9f), 1000) == Face::Cold);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 900, 70.0f), 1000) == Face::Idle);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 900, 104.9f), 1000) == Face::Idle);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 900, 105.0f), 1000) == Face::Hot);
}

// 红区压过水温异常:正在拉转速时,驾驶者要看的是转速,不是水温。
void test_redline_beats_coolant(void) {
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 6500, 120.0f), 1000) == Face::Redline);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(0, 6500, 20.0f), 1000) == Face::Redline);
}

// 冷车/过热压过巡航:这是"该被看见的异常",不能被普通行驶状态盖掉。
void test_coolant_anomaly_beats_cruise(void) {
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(60, 1500, 20.0f), 1000) == Face::Cold);
  face_reset();
  TEST_ASSERT_TRUE(face_update(mk(60, 1500, 120.0f), 1000) == Face::Hot);
}

void test_surprise_on_hard_accel(void) {
  face_reset();
  uint32_t t = 12000;
  TEST_ASSERT_TRUE(face_update(mk(10, 900), t) == Face::Idle);  // 铺垫
  t += 200;
  // 200ms 内 +30 km/h = 150 km/h/s → 惊喜
  TEST_ASSERT_TRUE(face_update(mk(40, 3000), t) == Face::Surprise);
  t += 200;
  TEST_ASSERT_TRUE(face_update(mk(40, 3000), t) == Face::Surprise);  // 持续期内
  t += 400;
  TEST_ASSERT_TRUE(face_update(mk(40, 3000), t) == Face::Cruise);  // 到期回落
}

void test_gentle_accel_no_surprise(void) {
  face_reset();
  uint32_t t = 13000;
  TEST_ASSERT_TRUE(face_update(mk(40, 2200), t) == Face::Cruise);
  t += 200;
  // 200ms 内 +1 km/h = 5 km/h/s → 不触发
  TEST_ASSERT_TRUE(face_update(mk(41, 2250), t) == Face::Cruise);
}

// 表情只跟车速/转速/水温,不依赖挡位(挡位留原表,不进屏)
void test_face_ignores_gear(void) {
  face_reset();
  uint32_t t = 13400;
  VehicleState a = mk(41, 2250);
  a.gear = Gear::P;
  VehicleState b = mk(41, 2250);
  b.gear = Gear::D;
  const Face fa = face_update(a, t);
  const Face fb = face_update(b, t + 200);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)fa, (uint8_t)fb);
}

// face_reset 必须真的清掉记忆:否则"换数据源"时,新旧源之间的速度差
// 会被当成急加速,开机就是一张惊喜脸。
void test_face_reset_clears_memory(void) {
  face_reset();
  face_update(mk(100, 6000), 5000);          // 让内部记住"上次 100 km/h"
  face_reset();
  // 复位后第一帧没有上一次速度可比 → 速度差为 0 → 不该惊喜
  TEST_ASSERT_TRUE(face_update(mk(0, 900), 6000) == Face::Idle);
}

void register_expression_tests(void) {
  RUN_TEST(test_face_base_states);
  RUN_TEST(test_rpm_alone_drives_face);
  RUN_TEST(test_coolant_drives_face);
  RUN_TEST(test_redline_beats_coolant);
  RUN_TEST(test_coolant_anomaly_beats_cruise);
  RUN_TEST(test_surprise_on_hard_accel);
  RUN_TEST(test_gentle_accel_no_surprise);
  RUN_TEST(test_face_ignores_gear);
  RUN_TEST(test_face_reset_clears_memory);
}

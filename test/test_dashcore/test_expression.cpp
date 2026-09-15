#include <unity.h>
#include "expression.h"

static VehicleState mk(float speed, float rpm) {
  VehicleState s;
  s.speed_kmh = speed;
  s.rpm = rpm;
  return s;
}

// 注意:face_update 是带静态状态的状态机,用例按顺序执行、速度平缓过渡
// (每步 Δv ≤ 15,即 ≤ 15 km/h/s,不触发惊喜),只验证基础表情与眨眼。
void test_face_base_states(void) {
  uint32_t t = 1000;
  TEST_ASSERT_TRUE(face_update(mk(0, 800), t) == Face::Idle);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(12, 1400), t) == Face::Idle);
  t += 1000;
  TEST_ASSERT_TRUE(face_update(mk(24, 2000), t) == Face::Blink);  // 3 秒周期眨眼
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

void test_surprise_on_hard_accel(void) {
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
  uint32_t t = 13000;
  TEST_ASSERT_TRUE(face_update(mk(40, 2200), t) == Face::Cruise);
  t += 200;
  // 200ms 内 +1 km/h = 5 km/h/s → 不触发
  TEST_ASSERT_TRUE(face_update(mk(41, 2250), t) == Face::Cruise);
}

// 表情只跟车速/转速,不依赖挡位(挡位留原表,不进屏)
void test_face_ignores_gear(void) {
  uint32_t t = 13400;
  VehicleState a = mk(41, 2250);
  a.gear = Gear::P;
  VehicleState b = mk(41, 2250);
  b.gear = Gear::D;
  const Face fa = face_update(a, t);
  const Face fb = face_update(b, t + 200);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)fa, (uint8_t)fb);
}

void register_expression_tests(void) {
  RUN_TEST(test_face_base_states);
  RUN_TEST(test_surprise_on_hard_accel);
  RUN_TEST(test_gentle_accel_no_surprise);
  RUN_TEST(test_face_ignores_gear);
}

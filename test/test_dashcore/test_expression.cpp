#include <unity.h>
#include <stdio.h>
#include "expression.h"

// ============================================================
// 表情状态机测试 —— 重点是"**每屏一套,各看各的表**"
//
// 左屏(转速表)只跟转速走,右屏(速度表)只跟车速走,水温两个都不参与。
// 所以每条用例都同时看**两张脸**:只动一路数据时,另一屏必须纹丝不动。
// 这正是用户试用时提的要求("独立出表情选项"),也是这一轮改动的主线。
// ============================================================

struct BothFaces { Face left; Face right; };

// 单帧判定:先清记忆(否则上一条用例的速度会串成"急加速"),
// 再喂一帧。now 取固定值 —— 表情是纯数据驱动的,时刻取多少都不该有影响。
static BothFaces at(float speed, float rpm, float coolant = 85.0f) {
  face_reset();
  VehicleState s;
  s.speed_kmh = speed;
  s.rpm = rpm;
  s.coolant_c = coolant;
  const FaceSet fs = face_update(s, 1000);
  return BothFaces{fs.left, fs.right};
}

// ---------------- 左屏:只看转速 ----------------
void test_left_follows_rpm_only(void) {
  // 转速分档边界(按实车地标定:怠速 900 / 巡航 2000 / 上限 6000)
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle,    (uint8_t)at(0, 1799).left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise,  (uint8_t)at(0, 1800).left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise,  (uint8_t)at(0, 3499).left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport,   (uint8_t)at(0, 3500).left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport,   (uint8_t)at(0, 4999).left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Redline, (uint8_t)at(0, 5000).left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Redline, (uint8_t)at(0, kRpmMax).left);

  // ★ 三个实车地标必须各自落在**对的那一档**里(这是用户给的真实数据,
  //   不是"随便取几个点"):怠速 900 → 常态、巡航 2000 → 巡航、上限 6000 → 红区
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)Face::Idle, (uint8_t)at(0, kRpmIdleNominal).left,
                                  "点火怠速 900 转该是常态脸");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)Face::Cruise, (uint8_t)at(0, kRpmCruiseNominal).left,
                                  "巡航 2000 转该是巡航脸");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)Face::Redline, (uint8_t)at(0, kRpmMax).left,
                                  "表盘上限 6000 转该是红区脸");
  // 红区必须在**上限之前**就开始提醒(踩到断油才亮红是没用的)
  TEST_ASSERT_TRUE_MESSAGE(at(0, kRpmMax - 1000.0f).left == Face::Redline,
                           "上限前 1000 转就该进红区");

  // ★ 车速从 0 扫到 210,左屏必须一直是同一个表情(转速不变就不许动)
  for (float v = 0; v <= kSpeedMax; v += 15.0f) {
    const BothFaces f = at(v, 3200.0f);
    char msg[80];
    snprintf(msg, sizeof(msg), "车速 %.0f 时左屏跟着变了(转速没变)", v);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)Face::Cruise, (uint8_t)f.left, msg);
  }
}

// ---------------- 右屏:只看车速 ----------------
void test_right_follows_speed_only(void) {
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle,   (uint8_t)at(29.9f, 900).right);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise, (uint8_t)at(30.0f, 900).right);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise, (uint8_t)at(89.9f, 900).right);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport,  (uint8_t)at(90.0f, 900).right);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport,  (uint8_t)at(kSpeedMax, 900).right);

  // ★ 转速从怠速扫到上限,右屏必须一直是常态(车速不变就不许动)
  for (float r = kRpmIdleNominal; r <= kRpmMax; r += 250.0f) {
    const BothFaces f = at(0, r);
    char msg[80];
    snprintf(msg, sizeof(msg), "转速 %.0f 时右屏跟着变了(车速没变)", r);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)Face::Idle, (uint8_t)f.right, msg);
  }
}

// ---------------- 水温:两屏都不参与 ----------------
void test_coolant_never_affects_faces(void) {
  // 同一组转速/车速下,水温从 20(冷启动)到 120(过热)扫一遍,两张脸都不许变
  const float kCoolants[] = {20.0f, 60.0f, 70.0f, 85.0f, 104.9f, 105.0f, 120.0f};
  const float kSpeeds[]   = {0.0f, 55.0f, 110.0f};
  const float kRpms[]     = {kRpmIdleNominal, kRpmCruiseNominal, kRpmMax};
  for (float c : kCoolants) {
    for (float v : kSpeeds) {
      for (float r : kRpms) {
        const BothFaces f = at(v, r, c);
        const BothFaces ref = at(v, r, 85.0f);
        char msg[96];
        snprintf(msg, sizeof(msg), "水温 %.1f 改变了表情(v=%.0f r=%.0f)", c, v, r);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)ref.left, (uint8_t)f.left, msg);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)ref.right, (uint8_t)f.right, msg);
      }
    }
  }
}

// ---------------- 两屏互不干扰(同一条数据上给出不同的脸) ----------------
void test_two_screens_can_differ(void) {
  // 高转速 + 低速:转速表已经"运动",速度表还是"常态"
  const BothFaces a = at(0, 4200);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)a.left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle,  (uint8_t)a.right);

  // 低转速 + 高速:反过来
  const BothFaces b = at(110, kRpmIdleNominal);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle,  (uint8_t)b.left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)b.right);

  // 巡航车速 + 巡航转速:两边都是"巡航",但这是各自的阈值各自命中的
  const BothFaces cruise = at(55, kRpmCruiseNominal);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise, (uint8_t)cruise.left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise, (uint8_t)cruise.right);

  // 两面都拉满(转速到表盘上限)
  const BothFaces c = at(120, kRpmMax);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Redline, (uint8_t)c.left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport,   (uint8_t)c.right);
}

// ---------------- 两个"专属"状态 ----------------
// 红区只属于左屏、惊喜只属于右屏 —— 这是 face_stages.h 里 kFaceRoleId
// 那一行 0 的依据,所以必须成立。
void test_redline_left_only_exhaustive(void) {
  for (float r = 0.0f; r <= kRpmMax; r += 50.0f) {
    const BothFaces f = at(0, r);
    TEST_ASSERT_TRUE_MESSAGE((uint8_t)f.right != (uint8_t)Face::Redline,
                             "右屏(速度表)不该出现红区");
  }
}

void test_surprise_right_only(void) {
  face_reset();
  // 怠速 → 200ms 内 +30 km/h = 150 km/h/s → 惊喜
  VehicleState a; a.speed_kmh = 10; a.rpm = kRpmIdleNominal; a.coolant_c = 85;
  face_update(a, 1000);
  VehicleState b; b.speed_kmh = 40; b.rpm = 4200; b.coolant_c = 85;
  const FaceSet fs = face_update(b, 1200);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Surprise, (uint8_t)fs.right);
  // ★ 左屏不受影响:惊喜是"车速的瞬态",而且左屏仍按当前转速给脸
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)fs.left);
}

void test_gentle_accel_no_surprise(void) {
  face_reset();
  VehicleState a; a.speed_kmh = 40; a.rpm = 2200; a.coolant_c = 85;
  face_update(a, 1000);
  // 200ms 内 +1 km/h = 5 km/h/s → 不触发
  VehicleState b; b.speed_kmh = 41; b.rpm = 2250; b.coolant_c = 85;
  const FaceSet fs = face_update(b, 1200);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise, (uint8_t)fs.right);
}

void test_surprise_expires(void) {
  face_reset();
  VehicleState a; a.speed_kmh = 10; a.rpm = 900; a.coolant_c = 85;
  face_update(a, 1000);
  VehicleState b; b.speed_kmh = 40; b.rpm = 900; b.coolant_c = 85;
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Surprise, (uint8_t)face_update(b, 1200).right);
  // 持续期内
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Surprise, (uint8_t)face_update(b, 1400).right);
  // 400ms 到期后回落到稳态(40 km/h → 巡航)
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise, (uint8_t)face_update(b, 1700).right);
}

// ---------------- 稳态与时间无关 ----------------
// 眨眼状态删掉之后,表情只由数据决定。这条同时防止有人再把
// "定时器驱动的表情"加回来。
void test_time_independent(void) {
  face_reset();
  VehicleState s; s.speed_kmh = 55; s.rpm = 900; s.coolant_c = 85;
  uint32_t t = 1000;
  for (int i = 0; i < 40; ++i) {           // 40 帧 × 500ms = 20 秒
    const FaceSet fs = face_update(s, t);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle, (uint8_t)fs.left);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise, (uint8_t)fs.right);
    t += 500;
  }
}

// 表情只跟转速/车速,不依赖挡位(挡位留原表,不进屏)
void test_face_ignores_gear(void) {
  face_reset();
  VehicleState a; a.speed_kmh = 41; a.rpm = 2250; a.coolant_c = 85;
  a.gear = Gear::P;
  VehicleState b = a;
  b.gear = Gear::D;
  const FaceSet fa = face_update(a, 1000);
  const FaceSet fb = face_update(b, 1200);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)fa.left, (uint8_t)fb.left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)fa.right, (uint8_t)fb.right);
}

// face_reset 必须真的清掉记忆:否则"换数据源"时,新旧源之间的速度差
// 会被当成急加速,开机就是一张惊喜脸。
void test_face_reset_clears_memory(void) {
  face_reset();
  VehicleState fast; fast.speed_kmh = 100; fast.rpm = 6000; fast.coolant_c = 85;
  face_update(fast, 5000);                  // 让内部记住"上次 100 km/h"
  face_reset();
  VehicleState slow; slow.speed_kmh = 0; slow.rpm = 900; slow.coolant_c = 85;
  const FaceSet fs = face_update(slow, 6000);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle, (uint8_t)fs.right);   // 不是惊喜
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle, (uint8_t)fs.left);
}

// 名称表:代码里用到名字的地方(串口日志)必须覆盖所有状态
void test_face_names(void) {
  TEST_ASSERT_EQUAL_STRING("idle", face_name(Face::Idle));
  TEST_ASSERT_EQUAL_STRING("cruise", face_name(Face::Cruise));
  TEST_ASSERT_EQUAL_STRING("sport", face_name(Face::Sport));
  TEST_ASSERT_EQUAL_STRING("redline", face_name(Face::Redline));
  TEST_ASSERT_EQUAL_STRING("surprise", face_name(Face::Surprise));
}

void register_expression_tests(void) {
  RUN_TEST(test_left_follows_rpm_only);
  RUN_TEST(test_right_follows_speed_only);
  RUN_TEST(test_coolant_never_affects_faces);
  RUN_TEST(test_two_screens_can_differ);
  RUN_TEST(test_redline_left_only_exhaustive);
  RUN_TEST(test_surprise_right_only);
  RUN_TEST(test_gentle_accel_no_surprise);
  RUN_TEST(test_surprise_expires);
  RUN_TEST(test_time_independent);
  RUN_TEST(test_face_ignores_gear);
  RUN_TEST(test_face_reset_clears_memory);
  RUN_TEST(test_face_names);
}

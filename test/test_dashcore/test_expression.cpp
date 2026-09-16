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

// 单帧判定:先清记忆(超速迟滞位),再喂一帧。
// now 取固定值 —— 表情是纯数据驱动的,时刻取多少都不该有影响。
static BothFaces at(float speed, float rpm, float coolant = 85.0f) {
  face_reset();
  VehicleState s;
  s.speed_kmh = speed;
  s.rpm = rpm;
  s.coolant_c = coolant;
  const FaceSet fs = face_update(s, 1000);
  return BothFaces{fs.left, fs.right};
}

// **不重置**记忆的连续喂帧 —— 迟滞测试必须连着喂(重置就把迟滞位清了)。
static Face right_of(float speed, uint32_t t) {
  VehicleState s;
  s.speed_kmh = speed;
  s.rpm = kRpmIdleNominal;
  s.coolant_c = 85;
  return face_update(s, t).right;
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
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport,  (uint8_t)at(130.0f, 900).right);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Overspeed, (uint8_t)at(kSpeedMax, 900).right);

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
// 红区只属于左屏、超速只属于右屏 —— 这是 face_stages.h 里 kFaceRoleId
// 那一行 0 的依据,所以必须成立。
void test_redline_left_only_exhaustive(void) {
  for (float r = 0.0f; r <= kRpmMax; r += 50.0f) {
    const BothFaces f = at(0, r);
    TEST_ASSERT_TRUE_MESSAGE((uint8_t)f.right != (uint8_t)Face::Redline,
                             "右屏(速度表)不该出现红区");
  }
}

void test_overspeed_right_only_exhaustive(void) {
  for (float v = 0.0f; v <= kSpeedMax; v += 5.0f) {
    const BothFaces f = at(v, kRpmIdleNominal);
    TEST_ASSERT_TRUE_MESSAGE((uint8_t)f.left != (uint8_t)Face::Overspeed,
                             "左屏(转速表)不该出现超速");
  }
}

// 超速档的边界:用户定的是"**大于** 130 km/h"。
// 130.0 不算、130.1 就算 —— 顺手把"阈值写成了 >= 还是 >"钉住。
void test_overspeed_threshold(void) {
  face_reset();
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)at(129.9f, kRpmIdleNominal).right);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)at(130.0f, kRpmIdleNominal).right);
  face_reset();   // 迟滞:上一条留在 130 时不构成"已经超速"
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Overspeed,
                          (uint8_t)at(130.1f, kRpmIdleNominal).right);
  // 满量程也该是超速(210 km/h 在表盘刻度上)
  face_reset();
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Overspeed, (uint8_t)at(kSpeedMax, 900).right);
}

// 超速是**稳态**:停在 140 上,时间过去多久都还是超速(与旧的"400ms 瞬态"相反)。
void test_overspeed_is_a_level_not_a_transient(void) {
  face_reset();
  VehicleState s; s.speed_kmh = 140; s.rpm = kRpmIdleNominal; s.coolant_c = 85;
  for (uint32_t t = 1000; t <= 5000; t += 250) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Overspeed, (uint8_t)face_update(s, t).right);
  }
  // 掉到 131 还是超速(仍在迟滞区上方),掉到 126 才退出
  VehicleState a = s; a.speed_kmh = 131;
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Overspeed, (uint8_t)face_update(a, 6000).right);
  VehicleState b = s; b.speed_kmh = 126;
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)face_update(b, 6100).right);
}

// 迟滞:127..130 这段"回退区"里保持上一状态,避免定速巡航压线时脸来回跳。
//   从下面上来(130.5 → 128)仍是超速;从上面掉下来停下(200 → 129)也是超速;
//   但**没超速过**的时候,129 不该判成超速。
void test_overspeed_hysteresis(void) {
  // ① 没进过超速:129 是运动
  face_reset();
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)right_of(129.0f, 1000));
  // ② 进过超速后掉到 128:保持超速(迟滞区 127..130 内不改判)
  face_reset();
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Overspeed, (uint8_t)right_of(140.0f, 2000));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Overspeed, (uint8_t)right_of(128.0f, 2100));
  // ③ 掉到 127 及以下:退出,回运动
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)right_of(127.0f, 2200));
}

// 一次"猛加速"不该改变任何东西 —— 这是旧"惊喜"档被删掉的原因,
// 留一条测试防止有人按加速度把瞬态加回来。
void test_acceleration_does_not_change_face(void) {
  face_reset();
  VehicleState a; a.speed_kmh = 10; a.rpm = kRpmIdleNominal; a.coolant_c = 85;
  face_update(a, 1000);
  // 200ms 内 +30 km/h = 150 km/h/s(旧实现会在这里亮"惊喜")
  VehicleState b; b.speed_kmh = 40; b.rpm = 4200; b.coolant_c = 85;
  const FaceSet fs = face_update(b, 1200);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise, (uint8_t)fs.right);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)fs.left);
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

// face_reset 必须真的清掉记忆(超速迟滞位):
// 否则"换数据源"时,上一个源还停在 140 km/h 会让新源在 127..130 之间
// 继续报超速 —— 明明已经慢下来,超速脸还挂着。
void test_face_reset_clears_memory(void) {
  face_reset();
  // 进过超速(140),再掉到 128:迟滞让它保持超速
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Overspeed, (uint8_t)right_of(140.0f, 5000));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Overspeed, (uint8_t)right_of(128.0f, 5100));
  // 清掉记忆后同样的 128 就该是运动
  face_reset();
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)right_of(128.0f, 5200));
  // 左屏不受影响(转速 900 = 常态)
  VehicleState slow; slow.speed_kmh = 0; slow.rpm = 900; slow.coolant_c = 85;
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle, (uint8_t)face_update(slow, 5300).left);
}

// 名称表:代码里用到名字的地方(串口日志)必须覆盖所有状态
void test_face_names(void) {
  TEST_ASSERT_EQUAL_STRING("idle", face_name(Face::Idle));
  TEST_ASSERT_EQUAL_STRING("cruise", face_name(Face::Cruise));
  TEST_ASSERT_EQUAL_STRING("sport", face_name(Face::Sport));
  TEST_ASSERT_EQUAL_STRING("redline", face_name(Face::Redline));
  TEST_ASSERT_EQUAL_STRING("overspeed", face_name(Face::Overspeed));
}

void register_expression_tests(void) {
  RUN_TEST(test_left_follows_rpm_only);
  RUN_TEST(test_right_follows_speed_only);
  RUN_TEST(test_coolant_never_affects_faces);
  RUN_TEST(test_two_screens_can_differ);
  RUN_TEST(test_redline_left_only_exhaustive);
  RUN_TEST(test_overspeed_right_only_exhaustive);
  RUN_TEST(test_overspeed_threshold);
  RUN_TEST(test_overspeed_is_a_level_not_a_transient);
  RUN_TEST(test_overspeed_hysteresis);
  RUN_TEST(test_acceleration_does_not_change_face);
  RUN_TEST(test_time_independent);
  RUN_TEST(test_face_ignores_gear);
  RUN_TEST(test_face_reset_clears_memory);
  RUN_TEST(test_face_names);
}

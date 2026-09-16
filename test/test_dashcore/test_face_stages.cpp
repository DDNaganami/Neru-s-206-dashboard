// 表情阶段表测试（宿主机）
//
// 这张表回答的是产品问题:"低/中/高转速、低/中/高水温、低/中/高速度"下,
// 屏幕上的表情到底换不换、换成哪一张?
//
// 为什么值得单独测:
//   · 网页端(表情导入页)的"阶段模拟"直接读这张表的**镜像**,
//     它说"高转速 = 红区脸",用户在真车上就该看到红区脸。表错了,
//     模拟就是在骗人 —— 而模拟是用户唯一能在刷图前看到的证据。
//   · 9 条用例里有 3 条(水温那组)是**水温驱动表情**这个新行为的唯一文档。
//   · 阈值很容易互相盖住:比如"转速中"那条如果把转速写成 2600,
//     它就同时满足了"运动"的下限,用例会红 —— 这正是想要的保护。
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "face_stages.h"
#include "expression.h"

// 一条用例的判定:必须**先清状态机记忆**,否则上一条用例的速度会串成
// "急加速"(face_update 用相邻两次调用的速度差判定惊喜),用例之间就会互相污染。
// now 取 1000:小于 3000 的首次眨眼时刻,所以不会撞上 Blink 这个瞬态。
static Face run_stage(const FaceStage& st) {
  face_reset();
  VehicleState s;
  s.rpm = st.rpm;
  s.speed_kmh = st.speed_kmh;
  s.coolant_c = st.coolant_c;
  return face_update(s, 1000);
}

// 9 条阶段用例逐条对账
static void test_stage_table(void) {
  TEST_ASSERT_EQUAL_UINT8(9, kFaceStageCount);
  for (uint8_t i = 0; i < kFaceStageCount; ++i) {
    const FaceStage& st = kFaceStages[i];
    const Face got = run_stage(st);
    if (got != st.expect) {
      // 把是哪条用例打出来 —— 光看"期望 X 得到 Y"根本定位不到阈值
      char msg[128];
      snprintf(msg, sizeof(msg), "%s/%s rpm=%.0f spd=%.0f cool=%.0f got=%s want=%s",
               st.group, st.level, st.rpm, st.speed_kmh, st.coolant_c,
               face_name(got), face_name(st.expect));
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

// 三条"低中高"必须真的给出**不同**的表情,否则这张表就白列了
// (用户的原话是"低中高下不同的表情展现")。
static void test_levels_differ_within_group(void) {
  const char* groups[3] = {"rpm", "coolant", "speed"};
  for (int g = 0; g < 3; ++g) {
    Face seen[3] = {Face::Count, Face::Count, Face::Count};
    int n = 0;
    for (uint8_t i = 0; i < kFaceStageCount && n < 3; ++i) {
      if (strcmp(kFaceStages[i].group, groups[g]) == 0) {
        seen[n++] = kFaceStages[i].expect;
      }
    }
    TEST_ASSERT_EQUAL_INT(3, n);
    for (int a = 0; a < 3; ++a) {
      for (int b = a + 1; b < 3; ++b) {
        if (seen[a] == seen[b]) {
          char msg[96];
          snprintf(msg, sizeof(msg), "%s 组里有两档用了同一张表情(%s)",
                   groups[g], face_name(seen[a]));
          TEST_FAIL_MESSAGE(msg);
        }
      }
    }
  }
}

// 降级链:每一行都得**从自己开始**,而且必须能一路退到 Idle(常态),
// 否则"只导入一张常态图"的用户会遇到某些状态无图可切。
static void test_fallback_chain(void) {
  for (uint8_t slot = 0; slot < kFaceSlotCount; ++slot) {
    TEST_ASSERT_EQUAL_INT8((int8_t)slot, kFaceFallback[slot][0]);
    bool reachesIdle = false;
    for (int k = 0; k < 4; ++k) {
      const int8_t s = kFaceFallback[slot][k];
      TEST_ASSERT_TRUE(s >= 0 && s < (int8_t)kFaceSlotCount);
      if (s == (int8_t)Face::Idle) reachesIdle = true;
    }
    TEST_ASSERT_TRUE(reachesIdle);
    // 链里不能有重复项(重复 = 白占一格,真正的备选被挤掉了)
    for (int a = 0; a < 4; ++a) {
      for (int b = a + 1; b < 4; ++b) {
        TEST_ASSERT_TRUE(kFaceFallback[slot][a] != kFaceFallback[slot][b]);
      }
    }
  }
}

// 表里的槽位编号必须就是 Face 的枚举值(网页端按下标找角色,错一位全乱)
static void test_slot_index_equals_face(void) {
  TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)Face::Idle);
  TEST_ASSERT_EQUAL_UINT8(1, (uint8_t)Face::Blink);
  TEST_ASSERT_EQUAL_UINT8(2, (uint8_t)Face::Cruise);
  TEST_ASSERT_EQUAL_UINT8(3, (uint8_t)Face::Sport);
  TEST_ASSERT_EQUAL_UINT8(4, (uint8_t)Face::Redline);
  TEST_ASSERT_EQUAL_UINT8(5, (uint8_t)Face::Surprise);
  TEST_ASSERT_EQUAL_UINT8(6, (uint8_t)Face::Cold);
  TEST_ASSERT_EQUAL_UINT8(7, (uint8_t)Face::Hot);
  TEST_ASSERT_EQUAL_UINT8(8, (uint8_t)Face::Count);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Count, kFaceSlotCount);
}

void register_face_stage_tests(void) {
  RUN_TEST(test_stage_table);
  RUN_TEST(test_levels_differ_within_group);
  RUN_TEST(test_fallback_chain);
  RUN_TEST(test_slot_index_equals_face);
}

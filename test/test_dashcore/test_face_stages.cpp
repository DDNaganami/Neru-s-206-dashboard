// 表情阶段表测试（宿主机）
//
// 这张表回答的是产品问题:"低/中/高/红区转速"、"低/中/高车速"这些阶段下,
// **左右两屏各自**该显示哪张表情?
//
// 为什么值得单独测:
//   · 网页端(表情导入页)的"阶段模拟"直接读这张表的**镜像**,
//     它说"转速·中 → 左屏巡航/右屏常态",用户在真车上就该看到这个。
//     表错了,模拟就是在骗人 —— 而模拟是用户刷图前唯一能看到的证据。
//   · "每屏一套独立表情"这件事**只能靠断言表达**:表里那些
//     "右屏恒为常态"的格子如果没人检查,写错了完全不会报错。
//   · 阈值很容易互相盖住,阶段表是对阈值的第二份独立描述。
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "face_stages.h"
#include "expression.h"

// 一条用例的判定:必须**先清状态机记忆** —— 否则上一条用例停在 140 km/h
// 会把超速迟滞位留给下一条(第 4 条速度用例就是 140),用例之间互相污染。
// now 取 1000:固定值即可 —— 表情只由数据决定,与时刻无关(有单测钉住)。
static FaceSet run_stage(const FaceStage& st) {
  face_reset();
  VehicleState s;
  s.rpm = st.rpm;
  s.speed_kmh = st.speed_kmh;
  s.coolant_c = st.coolant_c;
  return face_update(s, 1000);
}

// 阶段用例逐条对账(左右两屏都比)
// ★ 条数变了要改这里:4 转速 + 4 车速 + 3 水温 + 3 进气温度 = 14。
//   (车速从 3 条变 4 条,是"惊喜"改成"超速"时补的 —— 原来第 4 档是瞬态,
//    列不进表,用户试用时发现"这个档选不出来";
//    进气温度那 3 条也是同一个来路:先加了弧和读数却漏了阶段表,
//    用户当场发现"阶段里面缺失了进气温度低中高的选项"。)
static void test_stage_table(void) {
  TEST_ASSERT_EQUAL_UINT8(14, kFaceStageCount);
  for (uint8_t i = 0; i < kFaceStageCount; ++i) {
    const FaceStage& st = kFaceStages[i];
    const FaceSet got = run_stage(st);
    char msg[192];
    if (got.left != st.left) {
      snprintf(msg, sizeof(msg), "%s/%s 左屏 rpm=%.0f spd=%.0f cool=%.0f intake=%.0f got=%s want=%s",
               st.group, st.level, st.rpm, st.speed_kmh, st.coolant_c, st.intake_c,
               face_name(got.left), face_name(st.left));
      TEST_FAIL_MESSAGE(msg);
    }
    if (got.right != st.right) {
      snprintf(msg, sizeof(msg), "%s/%s 右屏 rpm=%.0f spd=%.0f cool=%.0f intake=%.0f got=%s want=%s",
               st.group, st.level, st.rpm, st.speed_kmh, st.coolant_c, st.intake_c,
               face_name(got.right), face_name(st.right));
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

// ★ 每条用例的水温/转速/车速/进气温度必须与**实测**结果自洽 —— 表是手写的,
//   写错一格就会被 run_stage 抓到(这条与 test_stage_table 互补:
//   那条查"表 ↔ 状态机",这条查"表自己有没有自相矛盾")。
static void test_stage_values_are_consistent(void) {
  for (uint8_t i = 0; i < kFaceStageCount; ++i) {
    const FaceStage& st = kFaceStages[i];
    // 转速组:速度必须是 0(否则速度表也会动)
    if (strcmp(st.group, "rpm") == 0) {
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, st.speed_kmh, "转速组的速度必须是 0");
    }
    // 速度组:转速必须是怠速(否则转速表也会动)
    if (strcmp(st.group, "speed") == 0) {
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(900.0f, st.rpm, "速度组的转速必须是怠速 900");
    }
    // 两个温度组:两屏表情都必须是不变的常态,而且**各动各的副表** ——
    // 水温组不该动进气温度、进气组不该动水温(否则看不出是哪条副弧在变)
    if (strcmp(st.group, "coolant") == 0 || strcmp(st.group, "intake") == 0) {
      TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle, (uint8_t)st.left);
      TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle, (uint8_t)st.right);
    }
    if (strcmp(st.group, "coolant") == 0) {
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(35.0f, st.intake_c, "水温组的进气温度必须是常温 35");
    }
    if (strcmp(st.group, "intake") == 0) {
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(85.0f, st.coolant_c, "进气组的正常水温必须是 85");
    }
    // 非转速组/非速度组也要把这两个钉在"正常值"上,否则点温度档时
    // 转速表或速度表会跟着动,而这正是用户明确要求过的"每组只变自己那一维"
    if (strcmp(st.group, "intake") == 0) {
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(900.0f, st.rpm, "进气组的转速必须是怠速 900");
      TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, st.speed_kmh, "进气组的速度必须是 0");
    }
  }
}

// ★★ "每屏一套独立表情"的可执行定义:
//    · 转速组的**右屏**必须恒为常态(车速一直是 0)
//    · 速度组的**左屏**必须恒为常态(转速一直是怠速)
//    · 两个温度组(水温 / 进气温度)两屏都恒为常态
//    同时被驱动的那一屏必须真的**变**(低/中/高给出不同的脸)。
static void test_only_own_gauge_moves_own_screen(void) {
  for (uint8_t i = 0; i < kFaceStageCount; ++i) {
    const FaceStage& st = kFaceStages[i];
    const FaceSet got = run_stage(st);
    char msg[128];
    if (strcmp(st.group, "rpm") == 0) {
      snprintf(msg, sizeof(msg), "转速·%s 让右屏(速度表)动了", st.level);
      TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)Face::Idle, (uint8_t)got.right, msg);
    } else if (strcmp(st.group, "speed") == 0) {
      snprintf(msg, sizeof(msg), "速度·%s 让左屏(转速表)动了", st.level);
      TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)Face::Idle, (uint8_t)got.left, msg);
    } else {
      // 温度组(水温 / 进气温度):两屏都不许动表情 ——
      // 它们只驱动"自己那条副弧 + 自己那个数字"
      snprintf(msg, sizeof(msg), "%s·%s 让表情动了(温度不参与表情)", st.group, st.level);
      TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)Face::Idle, (uint8_t)got.left, msg);
      TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)Face::Idle, (uint8_t)got.right, msg);
    }
  }
}

// 被驱动的那一屏,各档必须给出**不同**的表情(否则这张表白列)
static void test_driven_screen_differs_within_group(void) {
  const char* groups[2] = {"rpm", "speed"};
  for (int g = 0; g < 2; ++g) {
    Face seen[4];
    int n = 0;
    for (uint8_t i = 0; i < kFaceStageCount; ++i) {
      if (strcmp(kFaceStages[i].group, groups[g]) == 0) {
        seen[n++] = (g == 0) ? kFaceStages[i].left : kFaceStages[i].right;
      }
    }
    TEST_ASSERT_TRUE(n >= 3);
    for (int a = 0; a < n; ++a) {
      for (int b = a + 1; b < n; ++b) {
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
    for (int a = 0; a < 4; ++a) {
      for (int b = a + 1; b < 4; ++b) {
        TEST_ASSERT_TRUE(kFaceFallback[slot][a] != kFaceFallback[slot][b]);
      }
    }
  }
}

// ★ 转速组的四档必须是**实车地标**,不是随手取的数 ——
//   表里的值就是"车上真会出现的那一格",所以要能对上怠速/巡航/上限。
//   换车、换表时这条会红,提醒你把 vehicle_state.h 的地标常量一起改。
static void test_rpm_stages_use_real_landmarks(void) {
  const FaceStage* rows[4] = {nullptr, nullptr, nullptr, nullptr};
  for (uint8_t i = 0; i < kFaceStageCount; ++i) {
    if (strcmp(kFaceStages[i].group, "rpm") != 0) continue;
    if (strcmp(kFaceStages[i].level, "low") == 0) rows[0] = &kFaceStages[i];
    if (strcmp(kFaceStages[i].level, "mid") == 0) rows[1] = &kFaceStages[i];
    if (strcmp(kFaceStages[i].level, "high") == 0) rows[2] = &kFaceStages[i];
    if (strcmp(kFaceStages[i].level, "redline") == 0) rows[3] = &kFaceStages[i];
  }
  for (int i = 0; i < 4; ++i) TEST_ASSERT_NOT_NULL(rows[i]);

  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(kRpmIdleNominal, rows[0]->rpm,
                                  "转速·低 应该是点火怠速(实车 900)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(kRpmCruiseNominal, rows[1]->rpm,
                                  "转速·中 应该是稳定巡航(实车 2000)");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(kRpmMax, rows[3]->rpm,
                                  "转速·红区 应该是表盘上限(实车 7000)");
  // 运动档取巡航与上限之间,而且要真的落在"运动"那一档
  TEST_ASSERT_TRUE(rows[2]->rpm > kRpmCruiseNominal);
  TEST_ASSERT_TRUE(rows[2]->rpm < kRpmMax);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Sport, (uint8_t)rows[2]->left);

  // 每个地标都必须落在**它自己那一档**里(表 ↔ 状态机的交叉验证)
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Idle, (uint8_t)run_stage(*rows[0]).left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Cruise, (uint8_t)run_stage(*rows[1]).left);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Redline, (uint8_t)run_stage(*rows[3]).left);
}

// 表里的槽位编号必须就是 Face 的枚举值(网页端按下标找角色,错一位全乱)
static void test_slot_index_equals_face(void) {
  TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)Face::Idle);
  TEST_ASSERT_EQUAL_UINT8(1, (uint8_t)Face::Cruise);
  TEST_ASSERT_EQUAL_UINT8(2, (uint8_t)Face::Sport);
  TEST_ASSERT_EQUAL_UINT8(3, (uint8_t)Face::Redline);
  TEST_ASSERT_EQUAL_UINT8(4, (uint8_t)Face::Overspeed);
  TEST_ASSERT_EQUAL_UINT8(5, (uint8_t)Face::Count);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Face::Count, kFaceSlotCount);
}

// ★ 每屏声明的状态集合 ↔ 角色编号表:两边必须严丝合缝。
//   不一致的后果是"某个状态永远显示不出来"或"某张图永远不会被用",
//   两种都不会报错 —— 所以必须机器校验。
static void test_screen_state_sets(void) {
  struct { uint8_t screen; const Face* list; const char* name; } kScreens[2] = {
    {0, kFaceLeftStates, "左屏(转速表)"},
    {1, kFaceRightStates, "右屏(速度表)"},
  };
  for (int i = 0; i < 2; ++i) {
    const uint8_t screen = kScreens[i].screen;
    bool listed[8] = {false};
    for (uint8_t k = 0; k < kFaceStatesPerScreen; ++k) {
      const uint8_t slot = (uint8_t)kScreens[i].list[k];
      TEST_ASSERT_TRUE(slot < kFaceSlotCount);
      listed[slot] = true;
      char msg[96];
      snprintf(msg, sizeof(msg), "%s 声明会产生 %s,但没有对应的图片角色号",
               kScreens[i].name, face_name(kScreens[i].list[k]));
      TEST_ASSERT_TRUE_MESSAGE(kFaceRoleId[screen][slot] != 0, msg);
    }
    // 没声明的状态必须是"不用"(0),否则那张图永远不会被显示
    for (uint8_t slot = 0; slot < kFaceSlotCount; ++slot) {
      if (listed[slot]) continue;
      char msg[96];
      snprintf(msg, sizeof(msg), "%s 不会产生 %s,却给了角色号 %u(那张图永远不会被用到)",
               kScreens[i].name, face_name((Face)slot), kFaceRoleId[screen][slot]);
      TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, kFaceRoleId[screen][slot], msg);
    }
  }
  // 每屏声明的状态数必须一样多(现在左右各 4 个 —— 图片是按"每屏 4 张"规划的)
  TEST_ASSERT_EQUAL_UINT8(4, kFaceStatesPerScreen);
  TEST_ASSERT_EQUAL_UINT8(4, (uint8_t)(sizeof(kFaceLeftStates) / sizeof(Face)));
  TEST_ASSERT_EQUAL_UINT8(4, (uint8_t)(sizeof(kFaceRightStates) / sizeof(Face)));
}

void register_face_stage_tests(void) {
  RUN_TEST(test_stage_table);
  RUN_TEST(test_stage_values_are_consistent);
  RUN_TEST(test_only_own_gauge_moves_own_screen);
  RUN_TEST(test_driven_screen_differs_within_group);
  RUN_TEST(test_fallback_chain);
  RUN_TEST(test_rpm_stages_use_real_landmarks);
  RUN_TEST(test_slot_index_equals_face);
  RUN_TEST(test_screen_state_sets);
}

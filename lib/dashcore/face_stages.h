#pragma once
#include <stdint.h>
#include "expression.h"

// ============================================================
// 表情阶段表 —— 固件与编辑器共用的**唯一事实来源**
//
// 这个文件回答四个问题:
//   1. "转速低/中/高/红区"和"速度低/中/高"这些阶段,
//      **左右两屏各自**该显示哪张表情?(kFaceStages)
//   2. 每屏实际会产生哪几个状态?(kFaceLeftStates / kFaceRightStates)
//   3. 某个状态没有导入图片时,退到哪张?(kFaceFallback)
//   4. 槽位对应哪个图片角色编号?(kFaceRoleId)
//
// 网页端(tools/theme-editor/face-stages.js)是这里的**镜像**,
//   tools/theme-editor/test-face-stages.js 会解析本文件并逐字段比对,
//   所以两边不可能悄悄跑偏 —— 改了这里,Node 那条测试会红。
//
// ★ 表格的书写格式是**被解析的**:每行必须保持
//     {"group", "level", 转速, 速度, 水温, Face::左, Face::右},
//   两段字符串用双引号、数字用浮点、状态用 Face:: 前缀。
//   加行/改数字都可以,但别改成别的排版。
// ============================================================

// ---- 1. 阶段用例(每行同时给出左右两屏的期望) ----
//
// ★★ 两组硬规则,都有测试钉住:
//
// ① **每组只变自己那一维**:速度组固定 rpm=900(怠速)、转速组固定 speed=0、
//    非水温组固定水温 85。用户试用时抓出来的 ——
//    点"速度·中"时转速表不能跟着动,否则画面里两个表同时变,
//    根本看不出这一档改了什么。
//    (第一版把速度组的转速写成 1500/2600 想"借转速凑表情",被当场看出来。)
//
// ② **只有该屏自己的那一路能改它的表情**:转速组的右屏必须恒为常态、
//    速度组的左屏必须恒为常态;水温组**两屏都不许变**(水温不参与表情)。
//    这正是"每屏一套独立表情"的可执行定义。
//
// 未被测的那两路取"正常值":速度 0、水温 85(正常运行温度)、转速 900(怠速)。
// 瞬态(惊喜 Surprise)不在表里 —— 它不是稳态,由 test_expression.cpp 单独覆盖。
struct FaceStage {
  const char* group;      // "rpm" | "speed" | "coolant"
  const char* level;      // "low" | "mid" | "high" | "redline"
  float rpm;
  float speed_kmh;
  float coolant_c;
  Face left;              // 左屏(转速表)该显示哪张
  Face right;             // 右屏(速度表)该显示哪张
};

static const FaceStage kFaceStages[] = {
  // 转速:只驱动左屏;右屏恒为常态(车速一直是 0)
  {"rpm", "low", 800.0f, 0.0f, 85.0f, Face::Idle, Face::Idle},
  {"rpm", "mid", 3200.0f, 0.0f, 85.0f, Face::Cruise, Face::Idle},
  {"rpm", "high", 4800.0f, 0.0f, 85.0f, Face::Sport, Face::Idle},
  {"rpm", "redline", 6600.0f, 0.0f, 85.0f, Face::Redline, Face::Idle},
  // 车速:只驱动右屏;左屏恒为常态(转速一直是怠速)
  {"speed", "low", 900.0f, 0.0f, 85.0f, Face::Idle, Face::Idle},
  {"speed", "mid", 900.0f, 55.0f, 85.0f, Face::Idle, Face::Cruise},
  {"speed", "high", 900.0f, 110.0f, 85.0f, Face::Idle, Face::Sport},
  // 水温:两屏表情都不动,只影响水温弧与水温数字
  {"coolant", "low", 900.0f, 0.0f, 60.0f, Face::Idle, Face::Idle},
  {"coolant", "mid", 900.0f, 0.0f, 85.0f, Face::Idle, Face::Idle},
  {"coolant", "high", 900.0f, 0.0f, 115.0f, Face::Idle, Face::Idle},
};
static const uint8_t kFaceStageCount =
    (uint8_t)(sizeof(kFaceStages) / sizeof(kFaceStages[0]));

// ---- 2. 每屏实际会产生哪些状态 ----
// 这是"每屏一套独立表情"的**声明**:左屏只可能有这四个(没有惊喜),
// 右屏只可能有这四个(没有红区)。
// 测试会断言:列在这里的状态在 kFaceRoleId 里必须有角色号,
// 没列出来的必须是 0(不用)—— 两处一旦不一致就是"某个状态永远显示不出来"
// 或者"某张图永远不会被用",都不会报错,所以必须机器校验。
static const Face kFaceLeftStates[]  = {Face::Idle, Face::Cruise, Face::Sport, Face::Redline};
static const Face kFaceRightStates[] = {Face::Idle, Face::Cruise, Face::Sport, Face::Surprise};
static const uint8_t kFaceStatesPerScreen = 4;

// ---- 3. 缺图降级链 ----
// 一行一个状态,**下标 = (uint8_t)Face**(枚举顺序就是槽位顺序,见 expression.h)。
// 按顺序找第一张"这屏已经导入"的图。
// 例:Sport 那张没导入 → 用 Cruise;Cruise 也没有 → Idle。
// 左屏右屏各查各的:两套差分图不能互相顶替,否则角色会串。
// 链里出现"这屏用不到的状态"(比如左屏链里的 Surprise)也没有副作用 ——
// 那个槽位在这屏没有角色号,g_face_ok 恒为 false,会被直接跳过。
#define FACE_SLOT(f) ((int8_t)(uint8_t)Face::f)
static const int8_t kFaceFallback[5][4] = {
  {FACE_SLOT(Idle),     FACE_SLOT(Cruise),  FACE_SLOT(Sport),    FACE_SLOT(Redline)},
  {FACE_SLOT(Cruise),   FACE_SLOT(Idle),    FACE_SLOT(Sport),    FACE_SLOT(Redline)},
  {FACE_SLOT(Sport),    FACE_SLOT(Cruise),  FACE_SLOT(Redline),  FACE_SLOT(Idle)},
  {FACE_SLOT(Redline),  FACE_SLOT(Sport),   FACE_SLOT(Cruise),   FACE_SLOT(Idle)},
  {FACE_SLOT(Surprise), FACE_SLOT(Sport),   FACE_SLOT(Cruise),   FACE_SLOT(Idle)},
};
#undef FACE_SLOT
static const uint8_t kFaceSlotCount = 5;

// ---- 4. 表情槽位 → 图片角色编号 ----
// 下标 [屏][槽位]:屏 0 = 左(转速表),屏 1 = 右(速度表)。
// **0 = 这屏用不到这个状态**(左屏没有惊喜、右屏没有红区)。
// ★ 这些数必须与 lib/themetool/image_blob.h 的 ImageRole 完全一致 ——
//   错了不会崩,只会"右屏显示左屏的脸",所以由
//   test_image_blob.cpp → test_face_role_ids_match_stages 逐条比对。
static const uint16_t kFaceRoleId[2][5] = {
  // 左屏(转速表):Idle Cruise Sport Redline Surprise(不用)
  {3, 12, 13, 4, 0},
  // 右屏(速度表):Idle Cruise Sport Redline(不用) Surprise
  {6, 17, 18, 0, 8},
};

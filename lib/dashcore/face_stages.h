#pragma once
#include <stdint.h>
#include "expression.h"

// ============================================================
// 表情阶段表 —— 固件与编辑器共用的**唯一事实来源**
//
// 这个文件回答两个问题:
//   1. "低/中/高转速、低/中/高水温、低/中/高速度"这 9 个阶段,
//      固件到底该显示哪张表情?(kFaceStages,test_face_stages.cpp 逐条断言)
//   2. 某个状态没有导入图片时,退到哪张?(kFaceFallback)
//
// 网页端(tools/theme-editor/face-stages.js)是这里的**镜像**,
//   tools/theme-editor/test-face-stages.js 会解析本文件并逐字段比对,
//   所以两边不可能悄悄跑偏 —— 改了这里,Node 那条测试会红。
//
// ★ 表格的书写格式是**被解析的**:每行必须保持
//     {"group", "level", 转速, 速度, 水温, Face::状态},
//   两段字符串用双引号、数字用浮点、状态用 Face:: 前缀。
//   加行/改数字都可以,但别改成别的排版。
// ============================================================

// ---- 1. 9 条阶段用例(转速/水温/速度 × 低中高) ----
//
// ★★ 每组**只能变自己那一维**,另外两维必须固定(速度组固定 rpm=900 怠速、
//    水温组/转速组固定 speed=0)。这是产品要求,不是排版洁癖:
//    用户在阶段模拟里点"速度·中"时,画面里**转速表不应该跟着动** ——
//    否则他看到的是"两个表一起变",没法判断这一档到底改了什么。
//    test_face_stages.cpp 的 test_stage_groups_isolate_one_dimension 钉住这条。
//    曾经违反过:速度组把 rpm 写成 1500/2600(为了凑出巡航/运动脸),
//    结果点速度档时转速弧和转速表的表情一起变 —— 用户第一眼就看出来了。
//    其实速度≥30/≥90 这两条阈值**自己**就能给出巡航/运动,不需要借转速。
//
// 未被测的那两路取"正常值":速度 0、水温 85(正常运行温度)、转速 900(怠速)。
// 瞬态(惊喜 Surprise)不在表里 —— 它不是稳态,由 test_expression.cpp 单独覆盖。
struct FaceStage {
  const char* group;      // "rpm" | "coolant" | "speed"
  const char* level;      // "low" | "mid" | "high"
  float rpm;
  float speed_kmh;
  float coolant_c;
  Face expect;
};

static const FaceStage kFaceStages[] = {
  {"rpm", "low", 800.0f, 0.0f, 85.0f, Face::Idle},
  {"rpm", "mid", 3200.0f, 0.0f, 85.0f, Face::Cruise},
  {"rpm", "high", 6600.0f, 0.0f, 85.0f, Face::Redline},
  {"coolant", "low", 900.0f, 0.0f, 60.0f, Face::Cold},
  {"coolant", "mid", 900.0f, 0.0f, 85.0f, Face::Idle},
  {"coolant", "high", 900.0f, 0.0f, 115.0f, Face::Hot},
  {"speed", "low", 900.0f, 0.0f, 85.0f, Face::Idle},
  {"speed", "mid", 900.0f, 55.0f, 85.0f, Face::Cruise},
  {"speed", "high", 900.0f, 110.0f, 85.0f, Face::Sport},
};
static const uint8_t kFaceStageCount =
    (uint8_t)(sizeof(kFaceStages) / sizeof(kFaceStages[0]));

// ---- 2. 缺图降级链 ----
// 一行一个状态,**下标 = (uint8_t)Face**(枚举顺序就是槽位顺序,见 expression.h)。
// 按顺序找第一张"这屏已经导入"的图。
// 例:Sport 那张没导入 → 用 Cruise;Cruise 也没有 → Redline;再没有 → Idle。
// 左屏右屏各查各的:两套差分图不能互相顶替,否则角色会串。
#define FACE_SLOT(f) ((int8_t)(uint8_t)Face::f)
static const int8_t kFaceFallback[7][4] = {
  {FACE_SLOT(Idle),     FACE_SLOT(Cruise),   FACE_SLOT(Sport),    FACE_SLOT(Redline)},
  {FACE_SLOT(Cruise),   FACE_SLOT(Idle),     FACE_SLOT(Sport),    FACE_SLOT(Redline)},
  {FACE_SLOT(Sport),    FACE_SLOT(Cruise),   FACE_SLOT(Redline),  FACE_SLOT(Idle)},
  {FACE_SLOT(Redline),  FACE_SLOT(Sport),    FACE_SLOT(Surprise), FACE_SLOT(Idle)},
  {FACE_SLOT(Surprise), FACE_SLOT(Redline),  FACE_SLOT(Sport),    FACE_SLOT(Idle)},
  {FACE_SLOT(Cold),     FACE_SLOT(Idle),     FACE_SLOT(Cruise),   FACE_SLOT(Sport)},
  {FACE_SLOT(Hot),      FACE_SLOT(Surprise), FACE_SLOT(Redline),  FACE_SLOT(Idle)},
};
#undef FACE_SLOT
static const uint8_t kFaceSlotCount = 7;

// ---- 3. 表情槽位 → 图片角色编号 ----
// 下标 [屏][槽位]:屏 0 = 左(转速表),屏 1 = 右(速度表);槽位见上面的一行一状态。
// ★ 这些数必须与 lib/themetool/image_blob.h 的 ImageRole 完全一致 ——
//   错了不会崩,只会"右屏显示左屏的脸",所以由
//   test_image_blob.cpp → test_face_role_ids_match_stages 逐条比对。
// 编号从 11 开始接(不是 9):9/10/11/16 都是保留编号,不复用。
static const uint16_t kFaceRoleId[2][7] = {
  {3, 12, 13, 4, 5, 14, 15},    // 左屏:Idle Cruise Sport Redline Surprise Cold Hot
  {6, 17, 18, 7, 8, 19, 20},    // 右屏:同上
};

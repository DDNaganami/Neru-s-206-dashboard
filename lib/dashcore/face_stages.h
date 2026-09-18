#pragma once
#include <stdint.h>
#include "expression.h"

// ============================================================
// 表情阶段表 —— 固件与编辑器共用的**唯一事实来源**
//
// 这个文件回答四个问题:
//   1. "转速 5 档 / 车速 5 档 / 水温 3 档 / 进气温度 3 档"这些阶段,
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
//     {"group", "level", 转速, 速度, 水温, 进气温度, Face::左, Face::右},
//   两段字符串用双引号、数字用浮点、状态用 Face:: 前缀。
//   加行/改数字都可以,但别改成别的排版。
// ============================================================

// ---- 1. 阶段用例(每行同时给出左右两屏的期望) ----
//
// ★★ 两组硬规则,都有测试钉住:
//
// ① **每组只变自己那一维**:速度组固定 rpm=900(怠速)、转速组固定 speed=0、
//    非水温组固定水温 85、非进气组固定进气 35(正常行驶时的进气温度)。
//    用户试用时抓出来的 ——
//    点"速度·中"时转速表不能跟着动,否则画面里两个表同时变,
//    根本看不出这一档改了什么。
//    (第一版把速度组的转速写成 1500/2600 想"借转速凑表情",被当场看出来。)
//
// ② **只有该屏自己的那一路能改它的表情**:转速组的右屏必须恒为常态、
//    速度组的左屏必须恒为常态;水温组与进气温度组**两屏都不许变**
//    (两个温度都只驱动"自己的副弧 + 自己的数字")。
//    这正是"每屏一套独立表情"的可执行定义。
//
// 未被测的那几路取"正常值":车速 0、水温 85(正常运行温度)、转速 900(怠速)、
// 进气温度 35(常温行驶)。
//
// ★ 转速五档直接用**实车地标**(2026-09-18 按 TU5JP4 + AL4 重排):
//   怠速 900 / 稳定巡航 2000 / 运动 4200 / 高转 5200 / 红区 6200 ——
//   每一行都是车上真会出现的那一格(5200 ≈ 2 档 95km/h 全油门;
//   6200 就在断油点 6300 下方,1 档 60km/h 就能到)。表盘上限是 7000,
//   但**红区那一行不取 7000** —— 发动机根本到不了(断油 6300),
//   模拟里点了也没有现实对应。
// 车速五档:0(静止) / 45(市区) / 80(快速路) / 115(高速) / 140(超速;
//   故意离 130 的阈值有距离,免得"阈值一改用例就擦边")。
// 温度两组各三档:水温 60/85/115(冷机/正常/偏热),
// 进气 20/40/65(环境温度/常温行驶/堵车热浸)。
//
// ★ 每屏 5 个状态各自都有用例(转速 5 + 车速 5),外加温度两组各 3 条
//   "只动副表、不动表情"的用例 —— 共 16 条。
struct FaceStage {
  const char* group;      // "rpm" | "speed" | "coolant" | "intake"
  const char* level;      // "low" | "mid" | "high" | "vhigh" | "redline" | "over"
  float rpm;
  float speed_kmh;
  float coolant_c;
  float intake_c;
  Face left;              // 左屏(转速表)该显示哪张
  Face right;             // 右屏(速度表)该显示哪张
};

static const FaceStage kFaceStages[] = {
  // 转速:只驱动左屏;右屏恒为常态(车速一直是 0)
  // 五档 = 实车地标(怠速/巡航/运动/高转/红区),阈值见 expression.cpp
  {"rpm", "low", 900.0f, 0.0f, 85.0f, 35.0f, Face::Idle, Face::Idle},
  {"rpm", "mid", 2000.0f, 0.0f, 85.0f, 35.0f, Face::Cruise, Face::Idle},
  {"rpm", "high", 4200.0f, 0.0f, 85.0f, 35.0f, Face::Sport, Face::Idle},
  {"rpm", "vhigh", 5200.0f, 0.0f, 85.0f, 35.0f, Face::High, Face::Idle},
  {"rpm", "redline", 6200.0f, 0.0f, 85.0f, 35.0f, Face::Redline, Face::Idle},
  // 车速:只驱动右屏;左屏恒为常态(转速一直是怠速)
  {"speed", "low", 900.0f, 0.0f, 85.0f, 35.0f, Face::Idle, Face::Idle},
  {"speed", "mid", 900.0f, 45.0f, 85.0f, 35.0f, Face::Idle, Face::City},
  {"speed", "high", 900.0f, 80.0f, 85.0f, 35.0f, Face::Idle, Face::Cruise},
  {"speed", "vhigh", 900.0f, 115.0f, 85.0f, 35.0f, Face::Idle, Face::Sport},
  {"speed", "over", 900.0f, 140.0f, 85.0f, 35.0f, Face::Idle, Face::Overspeed},
  // 水温:两屏表情都不动,只影响水温弧与水温数字(左屏)
  {"coolant", "low", 900.0f, 0.0f, 60.0f, 35.0f, Face::Idle, Face::Idle},
  {"coolant", "mid", 900.0f, 0.0f, 85.0f, 35.0f, Face::Idle, Face::Idle},
  {"coolant", "high", 900.0f, 0.0f, 115.0f, 35.0f, Face::Idle, Face::Idle},
  // 进气温度:同样两屏表情都不动,只影响进气弧与进气数字(右屏)。
  // ★ 这一组是 2026-09 加的:用户拿到蓝牙 ELM327 实测 OBD 支持 010F 之后,
  //   先加了弧和读数,却漏了阶段表 —— 阶段模拟里没有"进气低/中/高"可点,
  //   于是那条新弧在模拟器里根本走不起来。补上。
  {"intake", "low", 900.0f, 0.0f, 85.0f, 20.0f, Face::Idle, Face::Idle},
  {"intake", "mid", 900.0f, 0.0f, 85.0f, 40.0f, Face::Idle, Face::Idle},
  {"intake", "high", 900.0f, 0.0f, 85.0f, 65.0f, Face::Idle, Face::Idle},
};
static const uint8_t kFaceStageCount =
    (uint8_t)(sizeof(kFaceStages) / sizeof(kFaceStages[0]));

// ---- 2. 每屏实际会产生哪些状态 ----
// 这是"每屏一套独立表情"的**声明**:左屏只可能有这五个(没有超速、没有市区),
// 右屏只可能有这五个(没有红区、没有高转)。
// 测试会断言:列在这里的状态在 kFaceRoleId 里必须有角色号,
// 没列出来的必须是 0(不用)—— 两处一旦不一致就是"某个状态永远显示不出来"
// 或者"某张图永远不会被用",都不会报错,所以必须机器校验。
// ★ 每屏的状态数组**必须写在一行里**:test-face-stages.js 是按行解析它的
//   (跨行会被截成前半截,症状是"解析出 4 个状态" —— 2026-09-18 踩过)。
static const Face kFaceLeftStates[]  = {Face::Idle, Face::Cruise, Face::Sport, Face::High, Face::Redline};
static const Face kFaceRightStates[] = {Face::Idle, Face::City, Face::Cruise, Face::Sport, Face::Overspeed};
static const uint8_t kFaceStatesPerScreen = 5;

// ---- 3. 缺图降级链 ----
// 一行一个状态,**下标 = (uint8_t)Face**(枚举顺序就是槽位顺序,见 expression.h)。
// 按顺序找第一张"这屏已经导入"的图。
// 例:Sport 那张没导入 → 用 Cruise;Cruise 也没有 → Idle。
// 左屏右屏各查各的:两套差分图不能互相顶替,否则角色会串。
// 链里出现"这屏用不到的状态"(比如左屏链里的 Overspeed)也没有副作用 ——
// 那个槽位在这屏没有角色号,g_face_ok 恒为 false,会被直接跳过。
//
// ★ 顺序不是随便排的,按"离它最近的表情"来:
//   High(高转) → 先退 Sport(转速感最接近),再 Redline,再 Cruise,最后 Idle;
//   City(市区) → 先退 Cruise(行驶感最接近),再 Idle(更慢),再 Sport。
#define FACE_SLOT(f) ((int8_t)(uint8_t)Face::f)
static const int8_t kFaceFallback[7][5] = {
  // Idle(怠速/静止)
  {FACE_SLOT(Idle),      FACE_SLOT(Cruise),  FACE_SLOT(Sport),    FACE_SLOT(High),    FACE_SLOT(City)},
  // Cruise(巡航/快速路)
  {FACE_SLOT(Cruise),    FACE_SLOT(Idle),    FACE_SLOT(Sport),    FACE_SLOT(High),    FACE_SLOT(City)},
  // Sport(运动/高速)
  {FACE_SLOT(Sport),     FACE_SLOT(Cruise),  FACE_SLOT(High),     FACE_SLOT(Redline), FACE_SLOT(Idle)},
  // Redline(红区,只有左屏)
  {FACE_SLOT(Redline),   FACE_SLOT(High),    FACE_SLOT(Sport),    FACE_SLOT(Cruise),  FACE_SLOT(Idle)},
  // Overspeed(超速,只有右屏)→ 退到运动(最接近的"开得快"),再一路退到常态
  {FACE_SLOT(Overspeed), FACE_SLOT(Sport),   FACE_SLOT(Cruise),   FACE_SLOT(Idle),    FACE_SLOT(City)},
  // High(高转,只有左屏)
  {FACE_SLOT(High),      FACE_SLOT(Sport),   FACE_SLOT(Redline),  FACE_SLOT(Cruise),  FACE_SLOT(Idle)},
  // City(市区,只有右屏)
  {FACE_SLOT(City),      FACE_SLOT(Cruise),  FACE_SLOT(Idle),     FACE_SLOT(Sport),   FACE_SLOT(Overspeed)},
};
#undef FACE_SLOT
static const uint8_t kFaceSlotCount = 7;

// ---- 4. 表情槽位 → 图片角色编号 ----
// 下标 [屏][槽位]:屏 0 = 左(转速表),屏 1 = 右(速度表)。
// **0 = 这屏用不到这个状态**(左屏没有超速/市区、右屏没有红区/高转)。
// ★ 这些数必须与 lib/themetool/image_blob.h 的 ImageRole 完全一致 ——
//   错了不会崩,只会"右屏显示左屏的脸",所以由
//   test_image_blob.cpp → test_face_role_ids_match_stages 逐条比对。
// ★ 槽位 4 = 超速,角色号 8 —— 这个号当年是"惊喜",**改名不改号**,
//   已经导出的 image.bin 不受影响(见 image_blob.h 的保留编号说明)。
// ★ 槽位 5 = 高转(左) / 槽位 6 = 市区(右),角色号 **21 / 22** ——
//   2026-09-18 新增,编号从 21 起接(2/5/7/9/10/11/14/15/16/19/20 是保留号,不复用)。
static const uint16_t kFaceRoleId[2][7] = {
  // 左屏(转速表):Idle Cruise Sport Redline Overspeed(不用) High  City(不用)
  {3, 12, 13, 4, 0, 21, 0},
  // 右屏(速度表):Idle Cruise Sport Redline(不用) Overspeed High(不用) City
  {6, 17, 18, 0, 8, 0, 22},
};

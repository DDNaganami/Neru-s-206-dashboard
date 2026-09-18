#include "expression.h"
#include "vehicle_state.h"
#include "face_ladder.h"

// ============================================================
// 分档阈值 —— **5 + 5**（2026-09-18 定，依据是这台车的实际活动范围）
//
// 为什么不按表盘量程分（7000 / 210）：
//   刻度里很大一段是这台车永远走不到的。TU5JP4（1.6 16V，110hp/5750rpm、
//   147Nm/4000rpm）+ AL4（4AT，2.72/1.50/1.00/0.71，3/4 档有锁止）实际是：
//     · 暖机怠速 750~850，冷车快怠速 ~1100~1250
//     · 日常全落在 1600~3400 转：4 档 60km/h≈1570、100≈2620、130≈3400；
//       3 档 90km/h≈3300（AL4 正常升档点在 2000~2500）
//     · 全油门才上得去：1 档 50≈5000、60≈6000；2 档 80≈4400、100≈5500
//     · **断油 6300（用户实测 2026-09-18）** → 红区从 5800 起，留 420 转提前量
//   于是分界**刻意避开日常落脚点**（1600~3400 之间一个分界都不放），
//   否则 4 档巡航会贴着边界抖；运动档从 3500 起，正好在日常上限之上。
//
// 转速五档（左屏）:
//   怠速  < 1500          —— 包住暖机 850 与冷车快怠速 1250，不抖
//   巡航  1500(1580 进 / 1420 退)
//   运动  3500(3580 进 / 3420 退)  —— 退出门槛 3420 特意留在 4 档 130km/h
//                                   (3404 转)之上：高速巡航不会停在"运动"档
//   高转  4500(4580 进 / 4420 退)
//   红区  5800(5880 进 / 5720 退)  —— **断油 6300(用户实测)**,留 420 转提前量
//
// 车速五档（右屏）:
//   静止/挪车 <30(33 进 / 27 退) / 市区 30(68 进 / 62 退) /
//   快速路 65(98 进 / 92 退) / 高速 95..130 / 超速 >130(<=127 退，单独一条迟滞)
//   65 与 95 都落在**限速值的空档**里（60↔70、90↔100），
//   常见定速巡航点（50/80/110/120）不会贴着边界 —— 再加 3km/h 迟滞兜底。
//
// ★ 这些数与 lib/dashcore/face_stages.h 里的**阶段表**是一对:
//   阶段表按这张表列了转速五档 + 车速五档 + 温度两组各三档,共 16 条用例，
//   每条同时断言**左右两屏**该显示哪张脸。改任何一个数,
//   test_face_stages.cpp / test-face-stages.js 会红,提醒你把阶段表和文档一起改。
//
// 为什么用"绝对物理值"而不是主题里的量程:量程是**表盘刻度**(可以改)，
// 而"6000 转该提醒了"是开车的事实,不该随表盘刻度漂移。
// 表盘刻度上限本身是 kRpmMax(见 vehicle_state.h),两者刻意分开:
// 弧画满的位置看表盘(7000),表情报红的位置看发动机(6000)。
// ============================================================
static const float kRpmCruiseFrom  = 1500.0f;   // 怠速 → 巡航
static const float kRpmSportFrom   = 3500.0f;   // 巡航 → 运动
static const float kRpmHighFrom    = 4500.0f;   // 运动 → 高转
static const float kRpmRedlineFrom = 5800.0f;   // 高转 → 红区(断油 6300 之前 420 转)
static const float kSpeedCityFrom = 30.0f;      // 静止/挪车 → 市区
static const float kSpeedFastFrom = 65.0f;      // 市区 → 快速路
static const float kSpeedHighFrom = 95.0f;      // 快速路 → 高速
static const float kSpeedOver     = 130.0f;     // 高速 → 超速(进入:> 130)
static const float kSpeedOverBack = 127.0f;     // 超速 → 高速(退出,迟滞)

// 迟滞量。为什么需要、该给多大:见 face_ladder.h 顶部(AL4 锁止/解锁、
// 定速巡航 ±1km/h)。转速那 100 转**小于**锁止造成的 200~300 转差值 ——
// 那一档靠**边界位置**躲开(3500 在日常上限 3400 之上),不靠迟滞硬扛;
// 迟滞只管"贴着边界微抖"和"换档瞬间的抖动"。
static const float kRpmHyst   = 80.0f;
static const float kSpeedHyst = 3.0f;

// ============================================================
// 关于"超速"这一档(替代了当年的"惊喜")
//
// 原来的第 4 档是 Surprise(急加速瞬态):相邻两次调用的速度差 > 15 km/h/s
// 就亮 400ms。两个问题:
//   ① **实车几乎不可能触发** —— 15 km/h/s ≈ 4.2 m/s²,206 CC 1.6 自动
//      0-100 约 11s(均值 ≈ 9 km/h/s),峰值也就在 12-13 上下。
//      也就是说这张图导进去基本永远不会亮。
//   ② **模拟器里选不出来** —— 它不是按车速分的档,而是按"速度的变化率",
//      阶段模拟的"车速低/中/高"三个按钮里没有它的位置。用户试用时
//      直接问"这个惊喜档测的时候根本选不出来,是做什么用的"。
//   于是改成**超速**(用户定:大于 130 km/h):这是个稳态,能被阶段表列出来、
//   能在模拟器里点出来、上高速也真的会遇到。
//
// 迟滞 130 → 127:定速巡航正好压在 130 上下 ±1 km/h 抖时,脸会一秒一变,
// 看着像坏了。进入用严格 > 130(用户原话"大于 130"),退出放到 <= 127。
// ============================================================

// 分档表:下标 = 档位,顺序必须与枚举里的语义一致(升序)。
// ★ 每屏**各有一套** —— 左屏用 High(高转)、右屏用 City(市区),
//   这就是"两屏共用槽位名 + 各自档位序列"的落地方式。
static const Face kTachLadder[5]  = {Face::Idle, Face::Cruise, Face::Sport,
                                     Face::High, Face::Redline};
static const Face kSpeedLadder[4] = {Face::Idle, Face::City, Face::Cruise,
                                     Face::Sport};
static const float kRpmThresholds[4]   = {kRpmCruiseFrom, kRpmSportFrom,
                                          kRpmHighFrom, kRpmRedlineFrom};
// ★ 车速阶梯只放前三条边界(30/65/95 → 4 档),**不含 130**:
//   超速那条由下面的 g_overspeed 位单独管(严格 >130 进 / <=127 退)。
//   两条机制都管 130 会互相打架 —— 实测就是"降到 127 该退出时,
//   阶梯因为 130-3=127 而把它留在超速档"(2026-09-18 用例抓出来的)。
static const float kSpeedThresholds[3] = {kSpeedCityFrom, kSpeedFastFrom,
                                          kSpeedHighFrom};

// 状态机的记忆:每条轴只记"上次在第几档" —— 迟滞全靠它(见 face_ladder.h)。
// 表情**只由数据决定、与时间无关**(有单测钉住),所以记忆里没有时间戳。
static uint8_t g_rpm_stage = 0;
static uint8_t g_speed_stage = 0;
static bool g_overspeed = false;   // 超速那一条的"严格 >130 / <=127"语义

// ---- 左屏(转速表):只看转速 ----
// 水温、车速都不参与 —— 这是"每屏一套独立表情"的核心。
static Face tach_face(float rpm) {
  g_rpm_stage = ladderStep(rpm, kRpmThresholds, 4, kRpmHyst, g_rpm_stage);
  return kTachLadder[g_rpm_stage];
}

// ---- 右屏(速度表):只看车速 ----
// 五档全部是**稳态**(看当前值),没有瞬态,所以这一屏也与时刻无关。
static Face speed_face(float speed_kmh) {
  // 超速那条保持原语义:严格 > 130 进、<= 127 退(比通用的 ±3 更宽一点 ——
  // 它是**警告**,宁可晚一点退,也不能在 128~129 来回闪)。
  if (speed_kmh > kSpeedOver)           g_overspeed = true;
  else if (speed_kmh <= kSpeedOverBack) g_overspeed = false;

  g_speed_stage = ladderStep(speed_kmh, kSpeedThresholds, 3, kSpeedHyst, g_speed_stage);
  Face f = kSpeedLadder[g_speed_stage];
  // 迟滞位与阶梯结论**取更严的那个**:车还在 128~130 之间时阶梯可能已经
  // 退回"高速",但警告应当继续亮到 127 以下。
  if (g_overspeed && (uint8_t)f < (uint8_t)Face::Overspeed) f = Face::Overspeed;
  return f;
}

FaceSet face_update(const VehicleState& s, uint32_t now) {
  // ★ now 现在**用不到**:眨眼删掉、惊喜(瞬态)改成超速(稳态)之后,
  //   表情完全由数据决定,与时刻无关(有单测钉住这一点)。
  //   参数保留是为了不动所有调用点,而且以后要加"持续 N 秒才报警"
  //   之类的瞬态,时间戳就从这里进来。
  (void)now;

  FaceSet out;
  // 左屏完全不看车速,右屏完全不看转速 —— 这是"每屏一套独立表情"的核心
  out.left  = tach_face(s.rpm);
  out.right = speed_face(s.speed_kmh);
  return out;
}

void face_reset() {
  // 状态有四个:两条轴各一个"上次在第几档" + 超速迟滞位。
  // 不清的话,换数据源时"上一秒还 140"会让新车速在 127..130 之间仍继续报超速;
  // 同理"上一秒 6500 转"会让新转速在 5800..6000 之间继续报红。
  g_rpm_stage = 0;
  g_speed_stage = 0;
  g_overspeed = false;
}

const char* face_name(Face f) {
  switch (f) {
    case Face::Idle: return "idle";
    case Face::Cruise: return "cruise";
    case Face::Sport: return "sport";
    case Face::Redline: return "redline";
    case Face::Overspeed: return "overspeed";
    case Face::High: return "high";
    case Face::City: return "city";
    default: return "?";
  }
}

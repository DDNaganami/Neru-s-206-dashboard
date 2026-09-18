#include "expression.h"
#include "vehicle_state.h"

// ============================================================
// 分档阈值
//
// ★ 转速三档按**实车地标**推(用户实测:点火怠速 900、稳定巡航 2000、
//   表盘上限 7000):
//     常态 < 1800  —— 包住怠速 900,并给起步/低速跟车留余量
//     巡航 1800..3499 —— 巡航 2000 稳稳落在里面(不会在 2000 上下抖来抖去)
//     运动 3500..4999 —— 加速、超车、上坡
//     红区 >= 5000 —— 但★ 这个值是按 6000 的表盘标的(见下),表盘更正为 7000 后偏早
//   所以红区**不是**"踩到断油才亮":5000 就该看到 —— 那才是它该提醒的位置。
//
// ★ 车速四档按 城区/快速路/高速/超速 取 30/90/130。
//
// ★ 这些数与 lib/dashcore/face_stages.h 里的**阶段表**是一对:
//   阶段表按这张表列了"转速低/中/高/红区"和"车速低/中/高/超速"共 8 条用例，
//   每条同时断言**左右两屏**该显示哪张脸。改任何一个数,
//   test_face_stages.cpp 会红,提醒你把阶段表和文档一起改 —— 别绕过。
//
// 为什么用"绝对物理值"而不是主题里的量程:量程是**表盘刻度**(可以改)，
// 而"5000 转该提醒了"是开车的事实,不该随表盘刻度漂移。
// 表盘刻度上限本身是 kRpmMax(见 vehicle_state.h),两者刻意分开:
// 弧画满的位置看表盘,表情报红的位置看发动机。
// ============================================================
static const float kRpmCruiseFrom  = 1800.0f;   // 转速·中(巡航)
static const float kRpmSportFrom   = 3500.0f;   // 转速·高(运动)
static const float kRpmRedlineFrom = 5000.0f;   // 红区(按 6000 表盘标的,待按 7000 重排)
static const float kSpeedMid   = 30.0f;         // 车速·中(巡航)
static const float kSpeedHigh  = 90.0f;         // 车速·高(运动)
static const float kSpeedOver  = 130.0f;        // 超速(进入:> 130)
static const float kSpeedOverBack = 127.0f;     // 超速(退出:<= 127,迟滞见下)

// ============================================================
// 关于"超速"这一档(第 4 档,替代了原来的"惊喜")
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
//   能在模拟器里点出来、上高速也真的会遇到。车速四档因此和转速四档对齐。
//
// 迟滞 130 → 127:定速巡航正好压在 130 上下 ±1 km/h 抖时,脸会一秒一变,
// 看着像坏了。进入用严格 > 130(用户原话"大于 130"),退出放到 <= 127。
// ============================================================
static bool g_overspeed = false;

// ---- 左屏(转速表):只看转速 ----
// 水温、车速都不参与 —— 这是"每屏一套独立表情"的核心。
// 分档按实车地标:怠速 900 落在常态、巡航 2000 落在巡航、红区在上限之前报红。
static Face tach_face(float rpm) {
  if (rpm >= kRpmRedlineFrom) return Face::Redline;
  if (rpm >= kRpmSportFrom)   return Face::Sport;
  if (rpm >= kRpmCruiseFrom)  return Face::Cruise;
  return Face::Idle;
}

// ---- 右屏(速度表):只看车速 ----
// 四档全部是**稳态**:超速也是(看当前车速),不再有瞬态,所以这一屏也和
// 时间无关。唯一的状态是超速的迟滞位 g_overspeed。
static Face speed_face(float speed_kmh) {
  if (speed_kmh > kSpeedOver)            g_overspeed = true;
  else if (speed_kmh <= kSpeedOverBack)  g_overspeed = false;
  if (g_overspeed)            return Face::Overspeed;
  if (speed_kmh >= kSpeedHigh) return Face::Sport;
  if (speed_kmh >= kSpeedMid)  return Face::Cruise;
  return Face::Idle;
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
  // 唯一的状态:超速迟滞位。不清的话,换数据源时"上一秒还 140"会让
  // 新车速在 127..130 之间仍继续报超速。
  g_overspeed = false;
}

const char* face_name(Face f) {
  switch (f) {
    case Face::Idle: return "idle";
    case Face::Cruise: return "cruise";
    case Face::Sport: return "sport";
    case Face::Redline: return "redline";
    case Face::Overspeed: return "overspeed";
    default: return "?";
  }
}

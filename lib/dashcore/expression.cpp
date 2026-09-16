#include "expression.h"
#include "vehicle_state.h"

// ============================================================
// 分档阈值
//
// ★ 转速三档按**实车地标**推(用户实测:点火怠速 900、稳定巡航 2000、
//   表盘上限 6000):
//     常态 < 1800  —— 包住怠速 900,并给起步/低速跟车留余量
//     巡航 1800..3499 —— 巡航 2000 稳稳落在里面(不会在 2000 上下抖来抖去)
//     运动 3500..4999 —— 加速、超车、上坡
//     红区 >= 5000 —— 上限 6000 之前 1000 转开始报红(83% 刻度)
//   所以红区**不是**"踩到断油才亮":5000 就该看到 —— 那才是它该提醒的位置。
//
// ★ 车速三档还没有实车数据,暂按 城区/快速路/高速 取 30/90。
//
// ★ 这些数与 test/test_dashcore/face_stages.h 里的**阶段表**是一对:
//   阶段表按这张表列了"转速低/中/高/红区"和"车速低/中/高"共 7 条用例，
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
static const float kRpmRedlineFrom = 5000.0f;   // 红区(上限 6000 之前 1000)
static const float kSpeedMid   = 30.0f;         // 车速·中
static const float kSpeedHigh  = 90.0f;         // 车速·高

// 急加速判定阈值(km/h 每秒)。
// 旧实现按"每帧 +25 km/h"判定,5Hz 渲染下等效 125 km/h/s,实际永不触发。
// 现在按时间归一:206 实测 0-100 约 11s(≈9 km/h/s 均值),
// 取 15 km/h/s ≈ 0.42g(弹射起步)作为惊喜阈值,假数据波峰也能偶尔触发。
static const float kSurpriseAccelPerSec = 15.0f;

static FaceState g_face{Face::Idle, 0};
static float prev_speed = 0.0f;
static uint32_t prev_now = 0;

// ---- 左屏(转速表):只看转速 ----
// 水温、车速都不参与 —— 这是"每屏一套独立表情"的核心。
// 分档按实车地标:怠速 900 落在常态、巡航 2000 落在巡航、上限 6000 前 1000 报红。
static Face tach_face(float rpm) {
  if (rpm >= kRpmRedlineFrom) return Face::Redline;
  if (rpm >= kRpmSportFrom)   return Face::Sport;
  if (rpm >= kRpmCruiseFrom)  return Face::Cruise;
  return Face::Idle;
}

// ---- 右屏(速度表):只看车速(稳态) ----
static Face speed_face(float speed_kmh) {
  if (speed_kmh >= kSpeedHigh) return Face::Sport;
  if (speed_kmh >= kSpeedMid)  return Face::Cruise;
  return Face::Idle;
}

FaceSet face_update(const VehicleState& s, uint32_t now) {
  const bool first_frame = (prev_now == 0);
  uint32_t dt_ms = first_frame ? 0 : (now - prev_now);
  prev_now = now;
  if (dt_ms > 1000) dt_ms = 1000;  // 停帧/时间回环保护

  float dv_s = 0.0f;
  if (dt_ms >= 10) dv_s = (s.speed_kmh - prev_speed) * 1000.0f / (float)dt_ms;
  prev_speed = s.speed_kmh;

  // 左屏完全不看瞬态:转速表不关心加速度(拉转速本身已经由转速档表达了)
  FaceSet out;
  out.left = tach_face(s.rpm);

  // 右屏的稳态 + 惊喜瞬态
  const Face base = speed_face(s.speed_kmh);
  if (dv_s > kSurpriseAccelPerSec) {
    g_face.face = Face::Surprise;
    g_face.until_ms = now + 400;
  } else if (now >= g_face.until_ms) {
    g_face.face = base;          // 稳态现算,不缓存
  }
  out.right = g_face.face;
  return out;
}

void face_reset() {
  g_face = FaceState{Face::Idle, 0};
  prev_speed = 0.0f;
  prev_now = 0;
}

const char* face_name(Face f) {
  switch (f) {
    case Face::Idle: return "idle";
    case Face::Cruise: return "cruise";
    case Face::Sport: return "sport";
    case Face::Redline: return "redline";
    case Face::Surprise: return "surprise";
    default: return "?";
  }
}

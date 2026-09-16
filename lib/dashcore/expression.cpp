#include "expression.h"
#include "vehicle_state.h"

// ============================================================
// 分档阈值
//
// ★ 这些数与 test/test_dashcore/face_stages.h 里的**阶段表**是一对:
//   阶段表按这张表列了"转速低/中/高/红区"和"速度低/中/高"共 7 条用例，
//   每条同时断言**左右两屏**该显示哪张脸。改任何一个数,
//   test_face_stages.cpp 会红,提醒你把阶段表和文档一起改 —— 别绕过。
//
// 为什么用"绝对物理值"而不是主题里的量程:量程是**表盘刻度**(可以改成 60~130)，
// 而"6000 转是红区"是发动机的事实,不该随表盘刻度漂移。
// ============================================================
static const float kRpmMid     = 2500.0f;   // 转速·中
static const float kRpmHigh    = 4500.0f;   // 转速·高
static const float kRpmRedline = 6000.0f;   // 红区
static const float kSpeedMid   = 30.0f;     // 车速·中
static const float kSpeedHigh  = 90.0f;     // 车速·高

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
static Face tach_face(float rpm) {
  if (rpm >= kRpmRedline) return Face::Redline;
  if (rpm >= kRpmHigh)    return Face::Sport;
  if (rpm >= kRpmMid)     return Face::Cruise;
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

#include "expression.h"
#include "vehicle_state.h"

// ============================================================
// 分档阈值 —— 三路数据各自的"低/中/高"
//
// ★ 这些数与 test/test_dashcore/face_stages.h 里的**阶段表**是一对:
//   阶段表按这张表列了 9 条用例(转速低中高 / 水温低中高 / 速度低中高)，
//   逐条断言 face_update 的输出。改任何一个数,test_face_stages.cpp 会红，
//   提醒你去把阶段表和文档一起改 —— 这是刻意的,别绕过。
//
// 为什么用"绝对物理值"而不是主题里的量程:量程是**表盘刻度**(可以改成 60~130)，
// 而"水温 105 度算过热"是发动机的事实,不该随表盘刻度漂移。
// ============================================================
static const float kRpmMid      = 2500.0f;   // 转速·中:巡航
static const float kRpmHigh     = 4500.0f;   // 转速·高:运动
static const float kRpmRedline  = 6000.0f;   // 红区:最优先(转速是驾驶者的第一意图)
static const float kSpeedMid    = 30.0f;     // 速度·中
static const float kSpeedHigh   = 90.0f;     // 速度·高
static const float kCoolantCold = 70.0f;     // 水温·低:暖机中
static const float kCoolantHot  = 105.0f;    // 水温·高:过热告警

// 急加速判定阈值(km/h 每秒)。
// 旧实现按"每帧 +25 km/h"判定,5Hz 渲染下等效 125 km/h/s,实际永不触发。
// 现在按时间归一:206 实测 0-100 约 11s(≈9 km/h/s 均值),
// 取 15 km/h/s ≈ 0.42g(弹射起步)作为惊喜阈值,假数据波峰也能偶尔触发。
static const float kSurpriseAccelPerSec = 15.0f;

static FaceState g_face{Face::Idle, 0};
static float prev_speed = 0.0f;
static uint32_t prev_now = 0;

// 稳态表情(不含惊喜这个瞬态)。
// 优先级从上到下 —— 越靠上越"该被看到":
//   红区 > 过热 > 冷车 > 运动 > 巡航 > 常态
// 红区压过水温异常:正在拉转速时,驾驶者要看的是转速。
static Face base_face(const VehicleState& s) {
  if (s.rpm >= kRpmRedline) return Face::Redline;
  if (s.coolant_c >= kCoolantHot) return Face::Hot;
  if (s.coolant_c < kCoolantCold) return Face::Cold;
  if (s.speed_kmh >= kSpeedHigh || s.rpm >= kRpmHigh) return Face::Sport;
  if (s.speed_kmh >= kSpeedMid || s.rpm >= kRpmMid) return Face::Cruise;
  return Face::Idle;
}

Face face_update(const VehicleState& s, uint32_t now) {
  const bool first_frame = (prev_now == 0);
  uint32_t dt_ms = first_frame ? 0 : (now - prev_now);
  prev_now = now;
  if (dt_ms > 1000) dt_ms = 1000;  // 停帧/时间回环保护

  float dv_s = 0.0f;
  if (dt_ms >= 10) dv_s = (s.speed_kmh - prev_speed) * 1000.0f / (float)dt_ms;
  prev_speed = s.speed_kmh;

  const Face base = base_face(s);

  // 惊喜:压过一切稳态,持续 400ms
  if (dv_s > kSurpriseAccelPerSec) {
    g_face.face = Face::Surprise;
    g_face.until_ms = now + 400;
    return g_face.face;
  }
  if (now < g_face.until_ms) {
    return g_face.face;      // 还在惊喜的持续期内
  }

  // 稳态:每次都按当前数据现算,不缓存
  g_face.face = base;
  return g_face.face;
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
    case Face::Cold: return "cold";
    case Face::Hot: return "hot";
    default: return "?";
  }
}

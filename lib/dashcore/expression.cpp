#include "expression.h"
#include "vehicle_state.h"

// 急加速判定阈值(km/h 每秒)。
// 旧实现按"每帧 +25 km/h"判定,5Hz 渲染下等效 125 km/h/s,实际永不触发。
// 现在按时间归一:206 实测 0-100 约 11s(≈9 km/h/s 均值),
// 取 15 km/h/s ≈ 0.42g(弹射起步)作为惊喜阈值,假数据波峰也能偶尔触发。
static const float kSurpriseAccelPerSec = 15.0f;

static FaceState g_face{Face::Idle, Face::Idle, 0};
static float prev_speed = 0.0f;
static uint32_t prev_now = 0;
static uint32_t next_blink_ms = 3000;

Face face_update(const VehicleState& s, uint32_t now) {
  uint32_t dt_ms = (prev_now == 0) ? 0 : (now - prev_now);
  prev_now = now;
  if (dt_ms > 1000) dt_ms = 1000;  // 停帧/时间回环保护

  float dv_s = 0.0f;
  if (dt_ms >= 10) dv_s = (s.speed_kmh - prev_speed) * 1000.0f / (float)dt_ms;
  prev_speed = s.speed_kmh;

  Face base = Face::Idle;
  if (s.rpm >= 6000.0f) {
    base = Face::Redline;
  } else if (s.speed_kmh >= 90.0f || s.rpm >= 4500.0f) {
    base = Face::Sport;
  } else if (s.speed_kmh >= 30.0f) {
    base = Face::Cruise;
  } else {
    base = Face::Idle;
  }

  if (dv_s > kSurpriseAccelPerSec) {
    g_face.face = Face::Surprise;
    g_face.base = base;
    g_face.until_ms = now + 400;
    return g_face.face;
  }

  if (now < g_face.until_ms) {
    return g_face.face;
  }

  if (base == Face::Idle && now >= next_blink_ms) {
    g_face.face = Face::Blink;
    g_face.base = Face::Idle;
    g_face.until_ms = now + 120;
    next_blink_ms = now + 3000 + (now % 2000);
    return g_face.face;
  }

  g_face.face = base;
  g_face.base = base;
  return g_face.face;
}

const char* face_name(Face f) {
  switch (f) {
    case Face::Idle: return "idle";
    case Face::Blink: return "blink";
    case Face::Cruise: return "cruise";
    case Face::Sport: return "sport";
    case Face::Redline: return "redline";
    case Face::Surprise: return "surprise";
    default: return "?";
  }
}

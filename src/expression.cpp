#include "expression.h"
#include "vehicle_state.h"

static FaceState g_face{Face::Idle, Face::Idle, 0};
static float prev_speed = 0;
static uint32_t next_blink_ms = 3000;

Face face_update(const VehicleState& s, uint32_t now) {
  const float dv = s.speed_kmh - prev_speed;
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

  if (dv > 25.0f) {
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

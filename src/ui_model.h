#pragma once
#include "vehicle_state.h"
#include "expression.h"

struct ArcDashView {
  float speed_t;
  float rpm_t;
  Gear gear;
  float coolant_c;
  float fuel_pct;
  uint8_t center_mode;
  float center_phase;
  Face face;
};

inline ArcDashView make_view(const VehicleState& s, uint32_t now_ms) {
  ArcDashView v{};
  v.speed_t = s.speed_kmh / kSpeedMax;
  v.rpm_t   = s.rpm / kRpmMax;
  if (v.speed_t < 0) v.speed_t = 0;
  if (v.speed_t > 1) v.speed_t = 1;
  if (v.rpm_t < 0) v.rpm_t = 0;
  if (v.rpm_t > 1) v.rpm_t = 1;

  v.gear = s.gear;
  v.coolant_c = s.coolant_c;
  v.fuel_pct = s.fuel_pct;
  v.center_mode = 1;
  v.center_phase = (now_ms % 2000) / 2000.0f;
  v.face = face_update(s, now_ms);
  return v;
}
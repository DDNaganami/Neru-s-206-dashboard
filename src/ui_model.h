#pragma once
#include "vehicle_state.h"
#include "expression.h"

struct ArcDashView {
  float speed_t;
  float rpm_t;
  float coolant_c;
  float fuel_pct;
  uint8_t center_mode;
  float center_phase;
  Face face;

  // 原始值(数字读数用)。
  // ★ 不要用 speed_t * kSpeedMax 反算:那是**百分比**,越界时被钳到 0/1,
  //   而且反算会多一次舍入。读数要显示的就是表里的原值。
  float speed_kmh;
  float rpm;
};

inline ArcDashView make_view(const VehicleState& s, uint32_t now_ms) {
  ArcDashView v{};
  v.speed_t = s.speed_kmh / kSpeedMax;
  v.rpm_t   = s.rpm / kRpmMax;
  if (v.speed_t < 0) v.speed_t = 0;
  if (v.speed_t > 1) v.speed_t = 1;
  if (v.rpm_t < 0) v.rpm_t = 0;
  if (v.rpm_t > 1) v.rpm_t = 1;

  v.coolant_c = s.coolant_c;
  v.fuel_pct = s.fuel_pct;
  v.speed_kmh = s.speed_kmh;
  v.rpm = s.rpm;
  v.center_mode = 1;
  v.center_phase = (now_ms % 2000) / 2000.0f;
  v.face = face_update(s, now_ms);
  return v;
}
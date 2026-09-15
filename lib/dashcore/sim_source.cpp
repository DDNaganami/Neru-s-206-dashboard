#include "sim_source.h"
#include <math.h>

void sim_update(VehicleState& s, uint32_t now_ms) {
  s.ign = true;

  const float t = now_ms / 1000.0f;
  const float wave = 0.5f * (sinf(t * 0.15f) + 1.0f);
  s.speed_kmh = wave * kSpeedMax;

  if (s.speed_kmh < 1) {
    s.gear = Gear::P;
    s.rpm = 800;
  } else if (s.speed_kmh < 10) {
    s.gear = Gear::D;
    s.rpm = 900 + s.speed_kmh * 40;
  } else {
    s.gear = Gear::D;
    s.rpm = 1200 + s.speed_kmh * 18;
    if (s.rpm > kRpmMax) s.rpm = kRpmMax;
  }

  s.coolant_c = 85 + 8 * sinf(t * 0.05f);
  s.fuel_pct = 70;
}

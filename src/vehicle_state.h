#pragma once
#include <stdint.h>

enum class Gear : uint8_t {
  P, R, N, D, M3, M2, M1
};

struct VehicleState {
  float speed_kmh = 0;
  float rpm = 0;
  float coolant_c = 20;
  float fuel_pct = 75;
  Gear gear = Gear::P;
  bool ign = true;
};

static constexpr float kSpeedMax = 210.0f;
static constexpr float kRpmMax = 7000.0f;

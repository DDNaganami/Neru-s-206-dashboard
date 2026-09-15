#pragma once
#include <stdint.h>
#include "vehicle_state.h"

enum class Face : uint8_t {
  Idle = 0,
  Blink,
  Cruise,
  Sport,
  Redline,
  Surprise,
  Count
};

struct FaceState {
  Face face;
  Face base;       // surprise/blink 结束后回到谁
  uint32_t until_ms;
};
Face face_update(const VehicleState& s, uint32_t now);
const char* face_name(Face f);
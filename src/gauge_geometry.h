#pragma once

// 原车积家白底，车速满刻度 210
// 角度等量了拆车表再改，先占位
struct GaugeGeometry {
  float speed_max = 210.0f;
  float speed_angle_start_deg = -135.0f;  // 待测
  float speed_angle_end_deg   =  135.0f;  // 待测

  float rpm_max = 7000.0f;
  float rpm_angle_start_deg = -135.0f;
  float rpm_angle_end_deg   =  135.0f;
};

inline float map_speed_to_angle(float kmh, const GaugeGeometry& g) {
  if (kmh < 0) kmh = 0;
  if (kmh > g.speed_max) kmh = g.speed_max;
  const float t = kmh / g.speed_max;
  return g.speed_angle_start_deg +
         t * (g.speed_angle_end_deg - g.speed_angle_start_deg);
}

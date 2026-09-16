#include "sim_source.h"
#include <math.h>

// 假数据源:没有硬件时驱动界面,同时也是**预览里唯一的"输入"**。
//
// ★ 转速按**表盘全量程**扫(怠速 → 上限),不是挂在车速上:
//   它的职责是"把界面每一档都演一遍",所以要让左屏的
//   常态/巡航/运动/红区 四个状态都会被走到。
//   旧版把转速绑在车速上(1200 + 速度×18),最高只到 4980 ——
//   红区(>=5000)永远演不到,预览里就看不到那张脸。
//   两个周期取互质一点的值(约 22s 与 42s),让组合能遍历得比较均匀。
// 车速仍按原来的慢波扫 0→kSpeedMax,右屏四档同理都会被走到。
void sim_update(VehicleState& s, uint32_t now_ms) {
  s.ign = true;

  const float t = now_ms / 1000.0f;

  // 车速:慢波扫满量程(周期约 42 秒)
  const float speedWave = 0.5f * (sinf(t * 0.15f) + 1.0f);
  s.speed_kmh = speedWave * kSpeedMax;
  s.gear = (s.speed_kmh < 1.0f) ? Gear::P : Gear::D;

  // 转速:另一个慢波(周期约 22 秒)扫 怠速 → 上限
  const float rpmWave = 0.5f * (sinf(t * 0.28f) + 1.0f);
  s.rpm = kRpmIdleNominal + rpmWave * (kRpmMax - kRpmIdleNominal);
  if (s.rpm > kRpmMax) s.rpm = kRpmMax;

  s.coolant_c = 85 + 8 * sinf(t * 0.05f);
  s.fuel_pct = 70;
}

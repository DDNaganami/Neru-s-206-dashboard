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
  Gear gear = Gear::P;  // 屏不显示挡位（原表负责），字段保留给将来逻辑
  bool ign = true;
};

static constexpr float kSpeedMax = 210.0f;

// 转速表**表盘刻度上限**(实车实测:上限 6000 转)。
//
// ★ 这个数是"弧画满时对应多少转",不是发动机的物理极限 ——
//   ui_model 用它算 rpm_t(弧的填充比例),所以必须等于表盘刻度上限,
//   否则弧永远填不满(设 7000 而上限 6000 → 最多只填到 86%)。
//   换表/换量程时改这里,表情的红区阈值另有一套(kRpmRedlineFrom),别混。
static constexpr float kRpmMax = 6000.0f;

// 实车转速地标(用户提供,用来定表情分档):
//   点火怠速 900 / 稳定巡航 2000 / 表盘上限 6000
static constexpr float kRpmIdleNominal   = 900.0f;
static constexpr float kRpmCruiseNominal = 2000.0f;

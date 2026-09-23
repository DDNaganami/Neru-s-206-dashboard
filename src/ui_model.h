#pragma once
#include "vehicle_state.h"
#include "expression.h"

struct ArcDashView {  float speed_t;
  float rpm_t;
  float coolant_c;
  // 进气温度(OBD 010F):速度表的副表,与转速表上的水温表对称。
  // 副表**不参与表情**,所以这里只有"值",没有对应的 Face 字段。
  float intake_c;
  float fuel_pct;
  uint8_t center_mode;
  float center_phase;

  // ★ 表情是**每屏一套**:左屏(转速表)只看转速,右屏(速度表)只看车速。
  //   所以这里是两个表情,不是一个 —— 传给 dash_ui 时按屏取。
  Face face_left;
  Face face_right;

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
  v.intake_c = s.intake_c;
  v.fuel_pct = s.fuel_pct;
  v.speed_kmh = s.speed_kmh;
  v.rpm = s.rpm;
  v.center_mode = 1;
  v.center_phase = (now_ms % 2000) / 2000.0f;
  const FaceSet fs = face_update(s, now_ms);
  v.face_left = fs.left;
  v.face_right = fs.right;
  return v;
}

// 指示灯槽位（六格灯）—— 定义在 lib/dashcore/lamp_view.h（纯映射、native 可测）。
// ★ 从 src/ui_model.h 里**移出去**了，不是删掉：这里 include 一下，
//   于是 dash_ui.cpp / main.cpp 照旧写 LampView / make_lamps / LampSlot，
//   一个调用点都不用改（那正是"移动而不是复制"的验收标准）。
#include "lamp_view.h"

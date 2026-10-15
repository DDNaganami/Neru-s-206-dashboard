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

// ★★ 2026-10-15：这里原来 include 了 `lib/dashcore/lamp_view.h`（六格指示灯的
//   槽位枚举 + 灯条几何 + `make_lamps()`），好让 dash_ui / main 照旧写
//   `LampView` / `LampSlot`。车主把整条灯栏从 UI 上删掉 ⇒ 那个头文件与这一行
//   一起删了；现在"告警 → 视图"只剩 `lib/dashcore/alert_view.h`（`AlertView`），
//   由 `dash_ui.h` include 进来。

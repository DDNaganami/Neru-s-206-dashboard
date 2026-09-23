#pragma once
#include "ui_model.h"

void dash_ui_init();
void dash_ui_tick(uint32_t now_ms);            // 每个主循环都调:LVGL 心跳
// 5Hz 由主循环节流。
// ★ 2026-09-24 加了第二个参数:`LampView`（指示灯槽位的六格）。
//   为什么**必填**而不是给个默认值：默认值会让"接线漏了一处"变成静默行为
//   （灯条永远全灭，看着像"车没这些状态"）。必填 = 漏传编译期就报出来。
void dash_ui_render(const ArcDashView& v, const LampView& lamps, uint32_t now_ms);

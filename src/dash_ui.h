#pragma once
#include "ui_model.h"

void dash_ui_init();
void dash_ui_tick(uint32_t now_ms);   // 每个主循环都调:LVGL 心跳
void dash_ui_render(const ArcDashView& v);  // 5Hz 由主循环节流

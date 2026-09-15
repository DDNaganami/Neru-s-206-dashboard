#pragma once
#include <lvgl.h>

// 显示驱动接口:注册两块 480×480 圆屏并返回。
// 当前为无屏桩驱动(DASH_DISPLAY_STUB=1):渲染进缓冲后丢弃,只用于逻辑联调。
// 屏到货后:在 dash_display.cpp 里加实驱动(ST7701S SPI / LovyanGFX),去掉桩。
void dash_display_init();
lv_display_t* dash_display_left();
lv_display_t* dash_display_right();
// 每个主循环调一次:设备上为空;pcpreview 里落 BMP 帧(见 preview/preview.html)
void dash_display_poll();

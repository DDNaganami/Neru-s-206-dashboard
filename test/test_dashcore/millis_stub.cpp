// Arduino millis() 桩:测试用全局变量控制时间。
#include "Arduino.h"

static uint32_t g_millis = 0;

uint32_t millis() { return g_millis; }
void test_set_millis(uint32_t ms) { g_millis = ms; }

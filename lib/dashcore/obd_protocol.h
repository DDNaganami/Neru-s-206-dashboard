#pragma once
#include <stdint.h>

// ELM327 文本协议纯解析层 —— 无 Arduino/LVGL 依赖,可在宿主机直接单元测试。
//
// 一行响应示例(ATH0 之后无 ECU 地址头):
//   "41 0C 1A F8"  → PID 0x0C(转速),原始值 0x1AF8
//   "41 05 3C"     → PID 0x05(水温),原始值 0x3C(单字节!)
//   带地址头时(如 "8F 41 05 3C")从 "41" 处开始仍能命中。
//
// 返回 true 表示解析出 PID 与数据;false = 不是数据帧或格式不完整。
bool parseObdLine(const char* line, uint8_t* pid_out, uint16_t* raw_out);

// 原始值 → 物理量
inline float rpmFromRaw(uint16_t raw) { return raw / 4.0f; }                       // 010C: (A*256+B)/4 rpm
inline float coolantFromRaw(uint16_t raw) { return (float)(raw & 0xFF) - 40.0f; }  // 0105: A-40 °C

#pragma once
#include <stdint.h>

// ELM327 文本协议纯解析层 —— 无 Arduino/LVGL 依赖,可在宿主机直接单元测试。
//
// 一行响应示例(ATH0 之后无 ECU 地址头):
//   "41 0C 1A F8"  → PID 0x0C(转速),原始值 0x1AF8
//   "41 05 3C"     → PID 0x05(水温),原始值 0x3C(单字节!)
//   "41 0F 2A"     → PID 0x0F(进气温度),原始值 0x2A(也是单字节)
//   带地址头时(如 "8F 41 05 3C")从 "41" 处开始仍能命中。
//
// 返回 true 表示解析出 PID 与数据;false = 不是数据帧或格式不完整。
// ★ 解析层**表驱动**(见 .cpp 的 kSupportedPids):只认轮询表里那几个 PID,
//   其它一律返回 false —— 包括 4100 的"支持位图",那不是数据。
//   加一个"01 服务 + 单字节"的 PID(比如这次的 010F 进气温度)要改两处:
//   这里的换算函数 + .cpp 的表。
bool parseObdLine(const char* line, uint8_t* pid_out, uint16_t* raw_out);

// 原始值 → 物理量
inline float rpmFromRaw(uint16_t raw) { return raw / 4.0f; }                       // 010C: (A*256+B)/4 rpm
inline float coolantFromRaw(uint16_t raw) { return (float)(raw & 0xFF) - 40.0f; }  // 0105: A-40 °C
// 010F 进气温度:换算与 0105 完全相同(A-40)。分开写一个名字是为了
// 调用点读起来是"进气温度",而不是"用冷却液的换算" —— 两者将来若分道扬镳
// (比如某个 ECU 用了两字节),改这里不会误伤冷却液。
inline float intakeFromRaw(uint16_t raw) { return (float)(raw & 0xFF) - 40.0f; }   // 010F: A-40 °C

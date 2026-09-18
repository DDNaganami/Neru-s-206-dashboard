#pragma once
#include <stdint.h>

// ELM327 文本协议纯解析层 —— 无 Arduino/LVGL 依赖,可在宿主机直接单元测试。
//
// 一行响应示例(ATH0 之后无 ECU 地址头):
//   "41 0C 1A F8"  → PID 0x0C(转速),原始值 0x1AF8
//   "41 05 3C"     → PID 0x05(水温),原始值 0x3C(单字节!)
//   "41 0F 2A"     → PID 0x0F(进气温度),原始值 0x2A(也是单字节)
//   "41 0D 3C"     → PID 0x0D(车速),原始值 0x3C = 60 km/h(单字节)
//   带地址头时(如 "8F 41 05 3C")从 "41" 处开始仍能命中。
//
// 返回 true 表示解析出 PID 与数据;false = 不是数据帧或格式不完整。
// ★ 解析层**表驱动**(见 .cpp 的 kSupportedPids):只认轮询表里那几个 PID,
//   其它一律返回 false —— 包括 4100 的"支持位图",那不是数据(它有自己的
//   解析函数 parseSupportedPids)。
//   加一个"01 服务 + 单字节"的 PID(比如这次的 010F 进气温度、010D 车速)
//   要改两处:这里的换算函数 + .cpp 的表。
bool parseObdLine(const char* line, uint8_t* pid_out, uint16_t* raw_out);

// 原始值 → 物理量
inline float rpmFromRaw(uint16_t raw) { return raw / 4.0f; }                       // 010C: (A*256+B)/4 rpm
inline float coolantFromRaw(uint16_t raw) { return (float)(raw & 0xFF) - 40.0f; }  // 0105: A-40 °C
// 010F 进气温度:换算与 0105 完全相同(A-40)。分开写一个名字是为了
// 调用点读起来是"进气温度",而不是"用冷却液的换算" —— 两者将来若分道扬镳
// (比如某个 ECU 用了两字节),改这里不会误伤冷却液。
inline float intakeFromRaw(uint16_t raw) { return (float)(raw & 0xFF) - 40.0f; }   // 010F: A-40 °C
// 010D 车速:单字节就是 km/h(A)。不需要钳制 —— 单字节最大 255,
// 而 255 km/h 对这台车本来就不可能(而且真超了也是"如实显示"更对)。
inline float speedFromRaw(uint16_t raw) { return (float)(raw & 0xFF); }            // 010D: A km/h

// ---- 0100:ECU 支持的 PID 位图 ----
//
// 响应形如 "41 00 BE 3E B8 13" —— 4 个字节 = 32 个位,从左到右依次是
// PID 0x01..0x20 的"支持/不支持"(MSB 在前)。它**不是数据**,parseObdLine
// 会(而且应该)拒掉它,所以单独一个函数。
//
// 为什么要它:车速那一路(010D)在 206 上到底有没有,靠"发了没人答"来试是有代价的
// —— ELM327 每个不存在的 PID 都要等到超时,而 K 线是一条排队共享的窄管子
// (见 obd_source.h 的"轮询时隙的账")。开机先问一次位图,就能**按 ECU 自己说的**
// 决定要不要占用那个时隙。
//   line:  一行响应(带 ECU 地址头也行)
//   mask:  bit31..bit0 ↔ PID 0x01..0x20
// 返回 true = 解析成功。
bool parseSupportedPids(const char* line, uint32_t* mask_out);

// mask 里 PID p 有没有置位(p 必须落在 0x01..0x20,否则一律 false)
bool pidSupported(uint32_t mask, uint8_t pid);

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ============================================================
// 路线几何（阶段 B）：手机把**一小段**路线点发过来，板子只负责画。
//
// 为什么另开一个魔数（0xA8）而不是改 v1 快照帧：
//   v1 的布局/黄金字节已经被 native 用例钉死，改它要动一堆判据；
//   路线是**新消息**，用独立魔数就等于"老帧原样、新帧追加"——
//   与 nav_proto.h 头里那条"只能往后加"的纪律一致。
//
// 帧格式（小端，CRC-16/CCITT-FALSE 覆盖到最后一个点）：
//   +0  magic 0xA8
//   +1  ver   1
//   +2  seq
//   +3  count 1..kNavRouteMax（超过就拒收）
//   +4.. +4+4*count-1 : count × (int16 dx, int16 dy)  —— 相对**车**的米数
//        dx 右为正、dy 前为负（屏坐标；与 LVGL 的 y 向下一致）
//   +.. crc16
//
// ★ 为什么用"相对车"而不是经纬度：板上没有地图数据，画出来的本来只是
//   "附近这段路"，用米数最省事（一条 24 点的弯道 = 96 字节，一个 BLE 包）。
//   手机侧由高德 SDK 的路线坐标 + 当前位置/航向换算成这个局部坐标。
// ============================================================

static const uint8_t kNavRouteMagic   = 0xA8u;
static const uint8_t kNavRouteVersion = 1u;
static const uint8_t kNavRouteMax     = 24u;

struct NavRoutePoint {
  int16_t dx = 0;   // 右为正（米）
  int16_t dy = 0;   // 前为负（米，屏坐标）
};

struct NavRoute {
  uint8_t       count = 0;
  uint8_t       seq = 0;
  NavRoutePoint pts[kNavRouteMax];
  bool          valid = false;
  uint32_t      updated_ms = 0;
};

static const size_t kNavRouteFrameMin = 4u + 4u + 2u;                        // 1 个点
static const size_t kNavRouteFrameMax = 4u + 4u * kNavRouteMax + 2u;         // 24 个点

// 打包：成功返回帧长，失败返回 0（点数越界/容量不够）
size_t nav_route_pack(const NavRoute& r, uint8_t* out, size_t cap);

// 解包：魔数/版本/点数/长度/CRC 全都要对；不合法一律 false
bool nav_route_unpack(const uint8_t* in, size_t len, NavRoute* out);

// 收到合法路线后上账（调用方保证 unpack 已成功）
inline void nav_route_apply(NavRoute& dst, const NavRoute& src, uint32_t now_ms) {
  dst = src;
  dst.valid = true;
  dst.updated_ms = now_ms;
}

// 超时降级：与 NavState 同一条口径（3 s 没有新路线 ⇒ 不再画）
inline bool nav_route_tick(NavRoute& r, uint32_t now_ms, uint32_t stale_ms = 3000u) {
  if (!r.valid) return false;
  if ((uint32_t)(now_ms - r.updated_ms) < stale_ms) return false;
  r.valid = false;
  return true;
}

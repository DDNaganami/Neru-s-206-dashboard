#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================
// 地图窗口（阶段 B3，魔数 0xAA）—— 抄 moto-gps-waveshare 的做法：
// **不发图片**，发"几条道路折线"，板子用矢量画布渲染。
//
// 一帧 = 多条折线（最多 kNavMapMaxLines 条，总点数 ≤ kNavMapMaxPoints）：
//   +0  magic 0xAA
//   +1  ver   1
//   +2  seq
//   +3  lines (1..kNavMapMaxLines)
//   +4  total_points (1..kNavMapMaxPoints)
//   +5  reserved
//   +6.. 每条线：{ count(u8), 之后 count 个 (int16 dx, int16 dy) }
//   ...  crc16（覆盖到最后一个点）
//
// 坐标与 0xA8 路线帧一致：**车体局部米**，dx 右为正、dy 前为负。
// 为什么和 0xA8 并存：0xA8 是"高亮路线"，0xAA 是"周围道路"；两者分开，
// 手机可以只发路线（省电）或同时发路网（更像地图）。
// ============================================================

static const uint8_t kNavMapMagic     = 0xAAu;
static const uint8_t kNavMapVersion   = 1u;
static const uint8_t kNavMapMaxLines  = 8u;
static const uint8_t kNavMapMaxPoints = 96u;

struct NavMapLine {
  uint8_t count = 0;
  int16_t dx[kNavMapMaxPoints > 40u ? 40u : kNavMapMaxPoints] = {0};   // 单条上限 40 点
  int16_t dy[kNavMapMaxPoints > 40u ? 40u : kNavMapMaxPoints] = {0};
};

struct NavMap {
  uint8_t  seq = 0;
  uint8_t  lines = 0;
  NavMapLine line[kNavMapMaxLines];
  bool     valid = false;
  uint32_t updated_ms = 0;
};

// 打包（lines/点数越界或容量不足返回 0）
size_t nav_map_pack(const NavMap& m, uint8_t* out, size_t cap);

// 解包（魔数/版本/条数/总点数/每条线长度/CRC 全校验）
bool nav_map_unpack(const uint8_t* in, size_t len, NavMap* out);

// 3 s 超时降级（与路线/状态同一条口径）
inline bool nav_map_tick(NavMap& m, uint32_t now_ms, uint32_t stale_ms = 3000u) {
  if (!m.valid) return false;
  if ((uint32_t)(now_ms - m.updated_ms) < stale_ms) return false;
  m.valid = false;
  return true;
}

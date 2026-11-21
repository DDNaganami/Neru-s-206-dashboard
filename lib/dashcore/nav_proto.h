#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================
// 导航短报文（手机 → 本机）—— **我们的**协议，不是高德的
//
// 为什么要有它：高德手机版发出的广播字段又多又散（`KEY_TYPE=10001` 里几十个
// extra），而且字段名/类型随版本变；ESP32 侧不该跟着它变。所以：
//   手机端 App 负责"把高德广播翻译成这一帧"，本机只认这一帧。
//   ⇒ 高德改字段，只改 Android 侧；固件/协议不动。
//
// 设计约束（2026-11-21 车主定方案时一起定的）：
//   · **一帧 15~38 字节**（含路名最多 23 字节 UTF-8）——2~5 Hz、BLE 上一帧就够；
//   · 定长头 + 变长路名 + CRC16；**小端**，与仓库其它协议一致；
//   · 不认识/坏帧一律**丢弃并计数**，绝不把半个字段喂给 UI（宁可显示"未连接"）；
//   · 魔数/版本不对就整帧丢 —— 将来加字段只能"追加到尾部 / 升版本"，不改已有偏移。
//
// ★ 字段来源（见 docs/BRANCH-NAV-FEASIBILITY.md §12 的高德广播映射）：
//   turn        ← `NEW_ICON`/`ICON`（编号见 NavTurn）
//   dist_m      ← `SEG_REMAIN_DIS_AUTO`
//   speed_kph   ← `CUR_SPEED`（高德给的当前车速；右屏自己的 OBD 车速是另一路）
//   heading_cdeg← `CAR_DIRECTION`（车头方向，0.01°，用于图片指针旋转）
//   remain_min  ← `ROUTE_REMAIN_TIME_AUTO`
//   speed_limit ← `LIMITED_SPEED`
//   flags.bit0  ← `KEY_TYPE=10019` 的 `EXTRA_STATE`（38 = 夜晚）⇒ 驱动夜间模式
//   road        ← `NEXT_ROAD_NAME`（UTF-8，截断到 kNavRoadMax 字节）
// ============================================================

// 高德的转向图标编号（社区实测；我们只用到这些，其余原样透传给 UI）
enum class NavTurn : uint8_t {
  Unknown  = 0,
  Left     = 2,
  Right    = 3,
  LeftAhead = 4,
  RightAhead = 5,
  UTurn    = 8,
  Straight = 9,
  Waypoint = 10,
  RampIn   = 11,
  RampOut  = 12,
  Arrive   = 15,
};

static const uint8_t  kNavProtoMagic   = 0xA7;
static const uint8_t  kNavProtoVersion = 1u;
static const uint8_t  kNavRoadMax      = 23u;    // UTF-8 字节数上限（不含结尾 0）
static const size_t   kNavFrameMin     = 16u;    // 头 14 + CRC 2（road_len=0）
static const size_t   kNavFrameMax     = 14u + kNavRoadMax + 2u;   // = 39

// flags 位
static const uint8_t kNavFlagNight      = 0x01u;   // 高德昼夜 = 夜晚
static const uint8_t kNavFlagNightValid = 0x02u;   // 这一帧带昼夜结论（否则不驱动夜间模式）
static const uint8_t kNavFlagArrived    = 0x04u;   // 已到达终点

struct NavFrame {
  uint8_t  turn        = 0;      // NavTurn 编号（未知值原样保留）
  uint16_t dist_m      = 0;      // 0..65535（饱和）
  uint8_t  speed_kph   = 0;      // 0..255
  uint16_t heading_cdeg = 0;     // 0..35999（0.01°）
  uint8_t  remain_min  = 0;      // 0..255
  uint8_t  speed_limit = 0;      // 0 = 未知
  uint8_t  flags       = 0;
  uint8_t  seq         = 0;      // 序号（回绕可见；重组/丢帧判据）
  uint8_t  road_len    = 0;
  // ★ 留 kNavRoadMax+1：线上最多传 kNavRoadMax 字节，但本地要能以 '\0' 结尾
  //   （UTF-8 文本交给 LVGL 必须是一个合法 C 字符串）。
  char     road[kNavRoadMax + 1] = {0};
};

// 打包到 out（容量 cap）。成功返回**帧长**，失败返回 0（容量不够 / road_len 越界）。
size_t nav_pack(const NavFrame& f, uint8_t* out, size_t cap);

// 解包：**任何**不合规都返回 false（魔数/版本/长度/road_len/CRC），
// 并尽量把 out 写成"未知"而不是半截数据（调用方应只在自己返回 true 时使用 out）。
bool nav_unpack(const uint8_t* in, size_t len, NavFrame* out);

// CRC-16/CCITT-FALSE（init 0xFFFF / poly 0x1021 / 不反射 / 不异或）。
// 单独暴露是为了 native 用例能钉住黄金字节。
uint16_t nav_crc16(const uint8_t* p, size_t len);

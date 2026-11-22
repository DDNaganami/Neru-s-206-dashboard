#pragma once
#include <stdint.h>
#include <string.h>

#include "nav_proto.h"
#include "nav_route.h"

// ============================================================
// 导航状态（纯数据 + 超时降级）—— 不碰 LVGL、不碰 BLE、不依赖 Arduino
//
// 为什么要单独一层：导航这条链路会**断**（手机没开高德 / App 被杀 / 蓝牙掉），
// 而"断了以后屏上该显示什么"必须是一条**确定的**判据，不能留给渲染层去猜：
//   · 超过 `stale_ms` 没收到好帧 ⇒ `valid = false`（UI 显示"导航未连接"，
//     而不是继续显示 5 分钟前的"前方 300 米右转"—— 那会害人）；
//   · 夜间模式只认**新鲜**的昼夜结论（`night_valid && valid`）；
//   · 坏帧不改变状态，只计数（调用方打日志）。
//
// ★ 与 `night_mode.h` / `panel_guard.h` 同一套路：判据是纯函数，native 用例钉住。
// ============================================================

struct NavTimeouts {
  uint32_t stale_ms = 3000;   // 2~5 Hz 的推送 ⇒ 3 s 没来就算断
};

struct NavState {
  bool     valid        = false;   // 现在的字段还能不能信
  bool     ever_received = false;  // 开机以来收到过好帧没有（区分"没连过"与"断线"）
  uint8_t  turn         = 0;       // NavTurn 编号（未知值原样保留）
  uint16_t dist_m       = 0;
  uint8_t  speed_kph    = 0;
  uint16_t heading_cdeg = 0;
  uint8_t  remain_min   = 0;
  uint8_t  speed_limit  = 0;
  bool     night        = false;   // 高德昼夜（10019）
  bool     night_valid  = false;   // 这一帧有没有带昼夜结论
  bool     arrived      = false;
  char     road[kNavRoadMax + 1] = {0};
  uint8_t  seq          = 255;     // 上一帧序号（255 = 还没收到过）
  uint32_t last_ms      = 0;
  NavRoute route;            // 阶段 B：路线几何（没收到就是 valid=false）
  uint32_t good_frames  = 0;
  uint32_t bad_frames   = 0;       // 由调用方在解包失败时 `++`
};

// 收到一帧**合法**数据（调用方只在 nav_unpack() 返回 true 时调）
inline void nav_state_apply(NavState& s, const NavFrame& f, uint32_t now_ms) {
  s.turn         = f.turn;
  s.dist_m       = f.dist_m;
  s.speed_kph    = f.speed_kph;
  s.heading_cdeg = (uint16_t)(f.heading_cdeg % 36000u);   // 防御：越界值只影响旋转角
  s.remain_min   = f.remain_min;
  s.speed_limit  = f.speed_limit;
  s.arrived      = (f.flags & kNavFlagArrived) != 0;
  if (f.flags & kNavFlagNightValid) {
    s.night_valid = true;
    s.night       = (f.flags & kNavFlagNight) != 0;
  }
  s.seq = f.seq;
  memcpy(s.road, f.road, sizeof(s.road));   // 两端都是 kNavRoadMax+1
  s.road[kNavRoadMax] = '\0';
  s.last_ms      = now_ms;
  s.valid        = true;
  s.ever_received = true;
  s.good_frames++;
}

// 每拍调用一次（渲染循环里调是安全的：只比较时间戳，不做 IO）。
// 返回 true = `valid` 这一拍翻转了（调用方据此重绘一次 / 打一行日志）。
// ★ 时间戳用无符号减法，32 位回绕安全。
inline bool nav_state_tick(NavState& s, uint32_t now_ms, const NavTimeouts& t) {
  if (!s.valid) return false;
  if ((uint32_t)(now_ms - s.last_ms) < t.stale_ms) return false;
  s.valid = false;
  return true;
}

// 夜间模式能不能用导航这一路（必须"收到过 + 有结论 + 还新鲜"）
inline bool nav_state_night_ok(const NavState& s) {
  return s.valid && s.night_valid;
}
inline bool nav_state_night_is_night(const NavState& s) { return s.night; }

// UI 用的"到路口距离"取值（纯数字，不掺中文，免得被字库卡住）：
//   · < 1000 m  ⇒ 返回**米**（整数），`use_km = false`，UI 画 "300" + "m"
//   · ≥ 1000 m  ⇒ 返回**0.1 km 的整数倍**，`use_km = true`，UI 画 "12" 后面自己点小数点
//   例：300 → (300,false)；999 → (999,false)；1000 → (10,true) 即 "1.0 km"；
//       1200 → (12,true) 即 "1.2 km"；1250 → (13,true)（四舍五入）
// 这样判别/取整都在纯函数里、可单测，UI 只负责摆位置。
inline uint32_t nav_dist_value(uint16_t dist_m, bool* use_km) {
  if (dist_m >= 1000u) {
    if (use_km) *use_km = true;
    return ((uint32_t)dist_m + 50u) / 100u;    // 米 → 0.1 km（四舍五入）
  }
  if (use_km) *use_km = false;
  return (uint32_t)dist_m;
}

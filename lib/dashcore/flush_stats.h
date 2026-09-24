#pragma once
#include <stdint.h>

// ============================================================
// "这一秒到底脏了多少" —— LVGL flush 区域的**每秒**统计（2026-09-24 新增）
//
// 为什么要有它：车主说"数字一跳，整块表像被刷了一刀"、"刷新发肉"，而这件事
// 在串口上原来**没有数字**可看：
//   · 既有的 `flush=` 是**累计值**（开机到现在），看不出"每秒刷几块"；
//   · 更看不出**最大的那一块有多大** —— 而"整屏重画"与"只重画读数带"的区别
//     恰恰只在那一块上。
// 这个结构补的就是这两格（判据口径与 `docs/RGB-PANEL-2.8C.md` §17 一致）：
//   · area_sum —— 本秒 LVGL 交给 flush 的区域**面积之和**（px²）
//                 ÷ 一屏面积 = "这一秒相当于把屏幕刷了几遍"；
//   · n        —— 本秒 flush 次数；
//   · max_w/max_h/max_area —— 本秒**单次最大**的那个矩形（w×h）与它的面积。
//
// ★ 为什么用"flush 区域之和"当 invalidate 面积：LVGL 只把**它认为脏了的**区域
//   交给 flush（`lv_refr.c` 的 `inv_areas[i]` → `call_flush_cb`）⇒ 这个和就是
//   "这一秒 LVGL 脏了多少像素"的直接读数，不必去 hook LVGL 内部。
// ★ 口径两条（写清楚，免得两个驱动各算一套）：
//   ① 面积 = (x2-x1+1)*(y2-y1+1)，即**矩形面积**（不是"非透明像素个数"）；
//   ② 每次 flush 调一次 `flush_stats_add()`；"最大"按**面积**比，平手保留先到的。
// ★ 这个头**不 include LVGL**（只吃宽高两个整数）：真机驱动、预览驱动两边
//   用的是逐字节同一份实现，将来要加 native 用例也不必多链接任何 .cpp。
// ============================================================

struct FlushStats {
  uint32_t area_sum;   // 本秒 flush 面积之和(px²)
  uint32_t n;          // 本秒 flush 次数
  int32_t  max_w;      // 单次最大矩形的宽(px)
  int32_t  max_h;      // 单次最大矩形的高(px)
  uint32_t max_area;   // 单次最大矩形的面积(px²)
};

// ★ 逐个字段赋值而不是 `s = FlushStats{}`：后者依赖聚合初始化的语言版本，
//   而这份头要同时喂给 gnu++ 的固件构建与宿主机构建（两边标准不同）。
inline void flush_stats_reset(FlushStats& s) {
  s.area_sum = 0u;
  s.n = 0u;
  s.max_w = 0;
  s.max_h = 0;
  s.max_area = 0u;
}

// 一次 flush 的矩形（宽/高，已裁剪过）。非正数的矩形直接不计。
inline void flush_stats_add(FlushStats& s, int32_t w, int32_t h) {
  if (w <= 0 || h <= 0) return;
  const uint32_t a = (uint32_t)w * (uint32_t)h;
  s.area_sum += a;
  ++s.n;
  if (a > s.max_area) {
    s.max_area = a;
    s.max_w = w;
    s.max_h = h;
  }
}

// 占比，单位 **0.1%**（打印时写 `%u.%u%%`）—— 特意不用浮点：
// 这两行会在每秒的诊断路径上打印，而整数除法在两条链上行为完全一致。
inline uint32_t flush_stats_pct_x10(uint32_t part, uint32_t whole) {
  if (whole == 0u) return 0u;
  return (uint32_t)(((uint64_t)part * 1000ull) / (uint64_t)whole);
}

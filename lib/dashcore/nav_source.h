#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nav_proto.h"
#include "nav_state.h"
#include "nav_route.h"
#include "nav_img.h"
#include "nav_map.h"

// ============================================================
// 导航字节流 → NavFrame → NavState（**传输无关**的源骨架）
//
// 为什么先做这一层：BLE 只是"一包一包字节"的管子。真正容易错的是
// **粘包/半包**（BLE 一次 notify 可能只给半帧，也可能两帧挤在一起）——
// 那部分与蓝牙无关，可以在这里用纯函数钉住，等到接 BLE 时只剩
// "把收到的字节喂进来"这一行。
//
// 用法（将来的 nav_source_ble 回调里）：
//     nav_source_push(g_nav_src, data, len, now_ms, state);
// 返回这一批字节里**成功上账**的帧数。
//
// ★ 缓冲策略：环形字节缓冲 128 B（最多 3 帧多一点的余量）。
//   · 找不到魔数 ⇒ 逐字节丢弃（噪音/串口日志混进来时不卡死）；
//   · 头部说 road_len=23 ⇒ 一帧 39 B；缓冲不够就等下一包；
//   · CRC 错 ⇒ 丢 1 字节再重新对齐（**不是**丢整缓冲：可能是"魔数撞车"，
//     真帧就在后面几个字节处）。
// ============================================================

static const size_t kNavSrcBuf = 320u;   // ★ 要放得下最大的单片：7 头 + 256 数据 + 2 CRC = 265

struct NavSource {
  uint8_t buf[kNavSrcBuf];
  size_t  len = 0;
  uint32_t crc_errors = 0;     // CRC 失败次数（诊断/日志用）
  uint32_t garbage_bytes = 0;  // 找不到魔数被丢掉的字节数
  uint32_t frames = 0;         // 成功解出的帧数
  NavImgRx* img = nullptr;     // 图片重组器（由 BLE 层提供缓冲；nullptr = 不接图片）
  uint32_t  img_ready = 0;     // 收齐的整图数量
  uint32_t  img_chunks = 0;    // 收到的合法分片数
  uint32_t  img_bad = 0;       // 分片解包失败次数
};

// 一帧在缓冲里够不够完整：先读 road_len（第 12 字节），算出总长
// 两种帧共用同一个环缓冲：按魔数分派（0xA7 = 导航快照，0xA8 = 路线几何）
// 0xAA 多折线帧的长度（按每条线的 count 累加，长度必须精确）
// ★★ 约定（2026-11-22 修正）：这三个按魔数算长度的函数
//   · 返回 `(size_t)-1` = **还不够**（上层 break 等下一批字节）
//   · 返回 `1`          = 头非法（上层丢 1 字节重新对齐）
//   · 返回 >1           = 完整帧长
//   以前"不够/非法"都返回 0，会被当成"不是这三种帧"而落到 0xA7 的布局去解
//   ⇒ 半包时丢字节、crc_err 每秒涨 1（实测就是这个现象）。
static inline size_t nav_map_frame_size(const uint8_t* p, size_t avail) {
  if (avail < 4u) return (size_t)-1;
  const uint8_t lines = p[3];
  if (lines == 0u || lines > kNavMapMaxLines) return 1u;
  size_t o = 6u;
  for (uint8_t i = 0; i < lines; ++i) {
    if (o >= avail) return (size_t)-1;
    const uint8_t c = p[o];
    if (c < 2u || c > 40u) return 1u;
    o += 1u + (size_t)c * 4u;
  }
  return o + 2u;
}

static inline size_t nav_frame_size_any(const uint8_t* p, size_t avail) {
  if (avail < 1u) return 0;
  if (p[0] == kNavMapMagic) return nav_map_frame_size(p, avail);
  if (p[0] == kNavImgMagic) {
    if (avail < 7u) return (size_t)-1;
    const uint16_t dlen = (uint16_t)((uint16_t)p[5] | ((uint16_t)p[6] << 8));
    if (dlen == 0u || dlen > kNavImgChunkMax) return 1u;
    return 7u + (size_t)dlen + 2u;
  }
  if (p[0] == kNavRouteMagic) {
    if (avail < 4u) return (size_t)-1;
    const uint8_t c = p[3];
    if (c == 0u || c > kNavRouteMax) return 1u;
    return 4u + (size_t)c * 4u + 2u;
  }
  return 0;   // 0xA7 走下面的旧函数
}

static inline size_t nav_frame_size_from_header(const uint8_t* p, size_t avail) {
  if (avail < 13u) return 0;          // 连 road_len 都还没到
  const uint8_t road_len = p[12];
  if (road_len > kNavRoadMax) return 0;
  return (size_t)14u + (size_t)road_len + 2u;
}

// 喂一批字节；解出的帧经 nav_state_apply 上账。
// 返回这一批里上账的帧数（0 = 还没凑够一帧 / 全是坏数据）。
inline size_t nav_source_push(NavSource& src, const uint8_t* data, size_t len,
                              uint32_t now_ms, NavState& st) {
  if (!data || len == 0) return 0;
  size_t applied = 0;

  for (size_t i = 0; i < len; ++i) {
    // 缓冲满：只可能是"半帧 + 后面还在等" ⇒ 丢最老的 1 字节保命（不会发生 39 < 128）
    if (src.len >= kNavSrcBuf) {
      memmove(src.buf, src.buf + 1, src.len - 1);
      --src.len;
    }
    src.buf[src.len++] = data[i];

    for (;;) {
      // 丢掉魔数之前的垃圾（并计数）
      size_t off = 0;
      while (off < src.len && src.buf[off] != kNavProtoMagic &&
             src.buf[off] != kNavRouteMagic && src.buf[off] != kNavImgMagic &&
             src.buf[off] != kNavMapMagic) ++off;
      if (off > 0) {
        memmove(src.buf, src.buf + off, src.len - off);
        src.len -= off;
        src.garbage_bytes += (uint32_t)off;
      }
      // ★ 2026-11-22：这里原来先卡 `src.len < kNavFrameMin`（16 = 快照帧最小长度），
      //   而路线帧最短只有 10 字节（1 点）⇒ 路线帧会被整帧跳过。现在先按魔数分派：
      //   路线帧自己算长度，快照帧才用 16 字节这条门槛。
      if (src.len < 4u) break;
      size_t need = nav_frame_size_any(src.buf, src.len);
      const bool is_route = (need != 0u);
      if (!is_route) {
        if (src.len < kNavFrameMin) break;
        need = nav_frame_size_from_header(src.buf, src.len);
      }
      if (need == 0) {   // 头部坏（road_len 越界）⇒ 丢一个字节重新对齐
        memmove(src.buf, src.buf + 1, src.len - 1);
        --src.len;
        ++src.crc_errors;
        continue;
      }
      if (src.len < need) break;   // 半包：等下一批

      bool ok = false;
      if (src.buf[0] == kNavImgMagic) {
        NavImgChunk c;
        if (src.img && nav_img_unpack_chunk(src.buf, need, &c)) {
          src.img_chunks++;
          if (nav_img_rx_push(*src.img, c)) src.img_ready++;
          ok = true;   // 分片本身合法就算"上账"（不要求整图齐）
        } else {
          src.img_bad++;
        }
      } else if (src.buf[0] == kNavMapMagic) {
        NavMap m;
        if (nav_map_unpack(src.buf, need, &m)) {
          m.valid = true;
          m.updated_ms = now_ms;
          st.map = m;
          ok = true;
        }
      } else if (is_route) {
        NavRoute r;
        if (nav_route_unpack(src.buf, need, &r)) {
          nav_route_apply(st.route, r, now_ms);
          ok = true;
        }
      } else {
        NavFrame f;
        if (nav_unpack(src.buf, need, &f)) {
          nav_state_apply(st, f, now_ms);
          ok = true;
        }
      }
      if (ok) {
        src.frames++;
        ++applied;
        memmove(src.buf, src.buf + need, src.len - need);
        src.len -= need;
        continue;
      }
      // CRC/版本不符：可能是魔数撞车 ⇒ 丢 1 字节再找
      memmove(src.buf, src.buf + 1, src.len - 1);
      --src.len;
      ++src.crc_errors;
    }
  }
  return applied;
}

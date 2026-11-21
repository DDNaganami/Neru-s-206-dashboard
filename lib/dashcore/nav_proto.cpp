#include "nav_proto.h"

#include <string.h>

// ---------------------------------------------------------------------------
// CRC-16/CCITT-FALSE
//
// 为什么不用现成的：仓库里只有 VAN 的 CRC15（`crc15_van_iso`），那是车辆协议的
// 定死口径、不能拿来给我们的应用层帧用。这里用教科书参数（0xFFFF / 0x1021），
// 不查表（39 字节的帧，逐位算足够快；省 512 字节 flash）。
// ---------------------------------------------------------------------------
uint16_t nav_crc16(const uint8_t* p, size_t len) {
  uint16_t crc = 0xFFFFu;
  if (!p) return crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)((uint16_t)p[i] << 8);
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

size_t nav_pack(const NavFrame& f, uint8_t* out, size_t cap) {
  if (!out) return 0;
  if (f.road_len > kNavRoadMax) return 0;
  const size_t n = 14u + (size_t)f.road_len + 2u;
  if (n > cap) return 0;

  out[0] = kNavProtoMagic;
  out[1] = kNavProtoVersion;
  out[2] = f.seq;
  out[3] = f.turn;
  out[4] = f.flags;
  out[5] = (uint8_t)(f.dist_m & 0xFFu);
  out[6] = (uint8_t)(f.dist_m >> 8);
  out[7] = f.speed_kph;
  out[8] = (uint8_t)(f.heading_cdeg & 0xFFu);
  out[9] = (uint8_t)(f.heading_cdeg >> 8);
  out[10] = f.remain_min;
  out[11] = f.speed_limit;
  out[12] = f.road_len;
  out[13] = 0;                      // 保留（将来加字段不改偏移）
  if (f.road_len) memcpy(out + 14, f.road, (size_t)f.road_len);

  const uint16_t crc = nav_crc16(out, n - 2u);
  out[n - 2u] = (uint8_t)(crc & 0xFFu);
  out[n - 1u] = (uint8_t)(crc >> 8);
  return n;
}

bool nav_unpack(const uint8_t* in, size_t len, NavFrame* out) {
  if (!in || !out) return false;
  if (len < kNavFrameMin || len > kNavFrameMax) return false;
  if (in[0] != kNavProtoMagic) return false;
  if (in[1] != kNavProtoVersion) return false;

  const uint8_t road_len = in[12];
  if (road_len > kNavRoadMax) return false;
  if (len != (size_t)14u + (size_t)road_len + 2u) return false;   // 长度必须**精确**匹配

  const uint16_t want = (uint16_t)((uint16_t)in[len - 2u] | ((uint16_t)in[len - 1u] << 8));
  if (nav_crc16(in, len - 2u) != want) return false;

  NavFrame f;
  f.seq          = in[2];
  f.turn         = in[3];
  f.flags        = in[4];
  f.dist_m       = (uint16_t)((uint16_t)in[5] | ((uint16_t)in[6] << 8));
  f.speed_kph    = in[7];
  f.heading_cdeg = (uint16_t)((uint16_t)in[8] | ((uint16_t)in[9] << 8));
  f.remain_min   = in[10];
  f.speed_limit  = in[11];
  f.road_len     = road_len;
  if (road_len) memcpy(f.road, in + 14, (size_t)road_len);
  f.road[road_len] = '\0';          // 缓冲留了 kNavRoadMax+1，这里最多写 index 23
  *out = f;
  return true;
}

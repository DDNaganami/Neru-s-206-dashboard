#include "nav_map.h"

#include "nav_proto.h"   // nav_crc16

size_t nav_map_pack(const NavMap& m, uint8_t* out, size_t cap) {
  if (!out) return 0;
  if (m.lines == 0u || m.lines > kNavMapMaxLines) return 0;

  size_t need = 6u;
  uint16_t total = 0;
  for (uint8_t i = 0; i < m.lines; ++i) {
    const uint8_t c = m.line[i].count;
    if (c < 2u || c > 40u) return 0;          // 一条线至少 2 点
    total = (uint16_t)(total + c);
    need += 1u + (size_t)c * 4u;
  }
  if (total == 0u || total > kNavMapMaxPoints) return 0;
  need += 2u;                                 // CRC
  if (need > cap) return 0;

  out[0] = kNavMapMagic;
  out[1] = kNavMapVersion;
  out[2] = m.seq;
  out[3] = m.lines;
  out[4] = (uint8_t)(total & 0xFFu);
  out[5] = 0;                                 // reserved

  size_t o = 6u;
  for (uint8_t i = 0; i < m.lines; ++i) {
    const NavMapLine& L = m.line[i];
    out[o++] = L.count;
    for (uint8_t p = 0; p < L.count; ++p) {
      const uint16_t ux = (uint16_t)L.dx[p];
      const uint16_t uy = (uint16_t)L.dy[p];
      out[o++] = (uint8_t)(ux & 0xFFu);
      out[o++] = (uint8_t)(ux >> 8);
      out[o++] = (uint8_t)(uy & 0xFFu);
      out[o++] = (uint8_t)(uy >> 8);
    }
  }
  const uint16_t crc = nav_crc16(out, o);
  out[o++] = (uint8_t)(crc & 0xFFu);
  out[o++] = (uint8_t)(crc >> 8);
  return o;
}

bool nav_map_unpack(const uint8_t* in, size_t len, NavMap* out) {
  if (!in || !out) return false;
  if (len < 8u) return false;                 // 6 头 + 至少 1 字节线 + 2 CRC 不对，这里只做粗检
  if (in[0] != kNavMapMagic || in[1] != kNavMapVersion) return false;

  const uint8_t lines = in[3];
  if (lines == 0u || lines > kNavMapMaxLines) return false;
  const uint16_t total = (uint16_t)in[4];
  if (total == 0u || total > kNavMapMaxPoints) return false;

  // 先算长度，再校验 CRC（长度必须精确，否则后面的解析全是错位）
  size_t need = 6u;
  size_t probe = 6u;
  uint16_t sum = 0;
  for (uint8_t i = 0; i < lines; ++i) {
    if (probe >= len) return false;
    const uint8_t c = in[probe];
    if (c < 2u || c > 40u) return false;
    sum = (uint16_t)(sum + c);
    if (sum > kNavMapMaxPoints) return false;
    probe += 1u + (size_t)c * 4u;
    if (probe > len) return false;
  }
  need = probe + 2u;
  if (len != need) return false;
  if (sum != total) return false;

  const uint16_t want = (uint16_t)((uint16_t)in[len - 2u] | ((uint16_t)in[len - 1u] << 8));
  if (nav_crc16(in, len - 2u) != want) return false;

  NavMap m;
  m.seq = in[2];
  m.lines = lines;
  size_t o = 6u;
  for (uint8_t i = 0; i < lines; ++i) {
    const uint8_t c = in[o++];
    m.line[i].count = c;
    for (uint8_t p = 0; p < c; ++p) {
      const uint16_t ux = (uint16_t)((uint16_t)in[o] | ((uint16_t)in[o + 1] << 8));
      const uint16_t uy = (uint16_t)((uint16_t)in[o + 2] | ((uint16_t)in[o + 3] << 8));
      m.line[i].dx[p] = (int16_t)ux;
      m.line[i].dy[p] = (int16_t)uy;
      o += 4u;
    }
  }
  *out = m;
  return true;
}

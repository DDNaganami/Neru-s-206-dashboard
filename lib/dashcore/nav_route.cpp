#include "nav_route.h"

#include "nav_proto.h"   // 复用 nav_crc16（同一条 CRC，别再造第二个）

size_t nav_route_pack(const NavRoute& r, uint8_t* out, size_t cap) {
  if (!out) return 0;
  if (r.count == 0u || r.count > kNavRouteMax) return 0;
  const size_t n = 4u + (size_t)r.count * 4u + 2u;
  if (n > cap) return 0;

  out[0] = kNavRouteMagic;
  out[1] = kNavRouteVersion;
  out[2] = r.seq;
  out[3] = r.count;
  size_t o = 4u;
  for (uint8_t i = 0; i < r.count; ++i) {
    const uint16_t ux = (uint16_t)r.pts[i].dx;
    const uint16_t uy = (uint16_t)r.pts[i].dy;
    out[o++] = (uint8_t)(ux & 0xFFu);
    out[o++] = (uint8_t)(ux >> 8);
    out[o++] = (uint8_t)(uy & 0xFFu);
    out[o++] = (uint8_t)(uy >> 8);
  }
  const uint16_t crc = nav_crc16(out, o);
  out[o++] = (uint8_t)(crc & 0xFFu);
  out[o++] = (uint8_t)(crc >> 8);
  return o;
}

bool nav_route_unpack(const uint8_t* in, size_t len, NavRoute* out) {
  if (!in || !out) return false;
  if (len < kNavRouteFrameMin || len > kNavRouteFrameMax) return false;
  if (in[0] != kNavRouteMagic) return false;
  if (in[1] != kNavRouteVersion) return false;
  const uint8_t count = in[3];
  if (count == 0u || count > kNavRouteMax) return false;
  if (len != 4u + (size_t)count * 4u + 2u) return false;   // 长度必须**精确**

  const uint16_t want = (uint16_t)((uint16_t)in[len - 2u] | ((uint16_t)in[len - 1u] << 8));
  if (nav_crc16(in, len - 2u) != want) return false;

  NavRoute r;
  r.seq = in[2];
  r.count = count;
  size_t o = 4u;
  for (uint8_t i = 0; i < count; ++i) {
    const uint16_t ux = (uint16_t)((uint16_t)in[o] | ((uint16_t)in[o + 1] << 8));
    const uint16_t uy = (uint16_t)((uint16_t)in[o + 2] | ((uint16_t)in[o + 3] << 8));
    r.pts[i].dx = (int16_t)ux;
    r.pts[i].dy = (int16_t)uy;
    o += 4u;
  }
  *out = r;
  return true;
}

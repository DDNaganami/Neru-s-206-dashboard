#include "nav_img.h"

#include "nav_proto.h"   // nav_crc16

size_t nav_img_pack_chunk(const NavImgChunk& c, uint8_t* out, size_t cap) {
  if (!out || !c.data) return 0;
  if (c.len == 0u || c.len > kNavImgChunkMax) return 0;
  if (c.total == 0u || c.total > kNavImgMaxChunks || c.idx >= c.total) return 0;
  const size_t n = 7u + (size_t)c.len + 2u;
  if (n > cap) return 0;

  out[0] = kNavImgMagic;
  out[1] = kNavImgVersion;
  out[2] = c.frame_id;
  out[3] = c.idx;
  out[4] = c.total;
  out[5] = (uint8_t)(c.len & 0xFFu);
  out[6] = (uint8_t)(c.len >> 8);
  memcpy(out + 7, c.data, c.len);
  const uint16_t crc = nav_crc16(out, 7u + c.len);
  out[7 + c.len] = (uint8_t)(crc & 0xFFu);
  out[8 + c.len] = (uint8_t)(crc >> 8);
  return n;
}

bool nav_img_unpack_chunk(const uint8_t* in, size_t len, NavImgChunk* out) {
  if (!in || !out) return false;
  if (len < 9u) return false;
  if (in[0] != kNavImgMagic || in[1] != kNavImgVersion) return false;
  const uint8_t total = in[4];
  if (total == 0u || total > kNavImgMaxChunks) return false;
  const uint8_t idx = in[3];
  if (idx >= total) return false;
  const uint16_t dlen = (uint16_t)((uint16_t)in[5] | ((uint16_t)in[6] << 8));
  if (dlen == 0u || dlen > kNavImgChunkMax) return false;
  if (len != 7u + (size_t)dlen + 2u) return false;
  const uint16_t want = (uint16_t)((uint16_t)in[len - 2u] | ((uint16_t)in[len - 1u] << 8));
  if (nav_crc16(in, len - 2u) != want) return false;

  out->frame_id = in[2];
  out->idx = idx;
  out->total = total;
  out->len = dlen;
  out->data = in + 7;
  return true;
}

void nav_img_rx_init(NavImgRx& rx, uint8_t* buf, size_t cap) {
  rx.buf = buf;
  rx.cap = (cap > kNavImgBufferMax) ? kNavImgBufferMax : cap;
  rx.frame_id = 0xFF;
  rx.total = 0;
  rx.got = 0;
  rx.len = 0;
  rx.complete = false;
  memset(rx.seen, 0, sizeof(rx.seen));
}

bool nav_img_rx_push(NavImgRx& rx, const NavImgChunk& c) {
  if (!rx.buf || rx.cap == 0u) return false;
  if (c.total == 0u || c.total > kNavImgMaxChunks || c.idx >= c.total) return false;
  if (c.len == 0u || c.len > kNavImgChunkMax) return false;
  if ((size_t)c.total * kNavImgChunkMax > rx.cap) return false;

  // 新的一帧：清空旧进度（不完整的旧图直接丢弃）
  if (c.frame_id != rx.frame_id) {
    rx.frame_id = c.frame_id;
    rx.total = c.total;
    rx.got = 0;
    rx.len = 0;
    rx.complete = false;
    memset(rx.seen, 0, sizeof(rx.seen));
  }

  const size_t bit = c.idx;
  if (rx.seen[bit >> 3] & (uint8_t)(1u << (bit & 7u))) {
    return false;   // 重复片：忽略（不报完成，避免同一张图被"收齐"两次）
  }
  const size_t off = (size_t)c.idx * kNavImgChunkMax;
  if (off + c.len > rx.cap) return false;
  memcpy(rx.buf + off, c.data, c.len);
  rx.seen[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
  rx.got++;
  if ((size_t)off + c.len > rx.len) rx.len = off + c.len;

  if (rx.got == rx.total) {
    rx.complete = true;
    return true;
  }
  return false;
}

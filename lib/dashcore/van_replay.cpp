#include "van_replay.h"

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool parseVanReplayLine(const char* line, VanPacket* pkt, uint32_t now_ms) {
  if (!line || !pkt) return false;

  const char* p = line;
  while (*p == ' ' || *p == '\t') ++p;
  if (!(p[0] == 'V' || p[0] == 'v') ||
      !(p[1] == 'A' || p[1] == 'a') ||
      !(p[2] == 'N' || p[2] == 'n')) return false;
  p += 3;

  // iden:恰好 3 位十六进制(12 位 VAN IDEN,写满)
  int iden = 0;
  for (int i = 0; i < 3; ++i) {
    while (*p == ' ' || *p == '\t') ++p;
    const int h = hexVal(*p);
    if (h < 0) return false;
    iden = (iden << 4) | h;
    ++p;
  }

  // 数据字节:2 位一组,最多 28 字节
  uint8_t len = 0;
  while (*p) {
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) break;
    const int hi = hexVal(p[0]);
    const int lo = (p[1] != '\0') ? hexVal(p[1]) : -1;
    if (hi < 0 || lo < 0) return false;
    if (len >= sizeof(pkt->data)) return false;
    pkt->data[len++] = (uint8_t)((hi << 4) | lo);
    p += 2;
  }

  pkt->iden = (uint16_t)iden;
  pkt->len = len;
  pkt->rx_ms = now_ms;
  return true;
}

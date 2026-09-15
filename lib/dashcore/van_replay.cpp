#include "van_replay.h"

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static const char* skipSpace(const char* p) {
  while (*p == ' ' || *p == '\t') ++p;
  return p;
}

// 帧格式:"VAN <iden> [cmd] <data...>"
//   iden:3~4 位十六进制(12 位或 15 位有效)。按 15 位解析:
//         3 位填 bit0..11,4 位填 bit0..15(高 3 位有效)
//   cmd :可选的单个十六进制位(4 位命令字段);省略时按协议约定取 EXT=1 → 0x8
//   data:2 位一组的十六进制字节
bool parseVanReplayLine(const char* line, VanPacket* pkt, uint32_t now_ms) {
  if (!line || !pkt) return false;

  const char* p = skipSpace(line);
  if (!(p[0] == 'V' || p[0] == 'v') ||
      !(p[1] == 'A' || p[1] == 'a') ||
      !(p[2] == 'N' || p[2] == 'n')) return false;
  p += 3;

  // iden:先读 3 位;只有在第 4 个十六进制位后面紧跟空白/结束时,
  // 才把它当作 4 位 iden 的一部分(否则它是数据的开头)。
  // 例:"VAN 1824 11 22" → 0x1824;"van82418f82710000000" → 0x824 + 数据。
  uint16_t iden = 0;
  int iden_digits = 0;
  p = skipSpace(p);
  for (int k = 0; k < 3; ++k) {
    const int h = hexVal(*p);
    if (h < 0) break;
    iden = (uint16_t)((iden << 4) | (uint16_t)h);
    ++iden_digits;
    ++p;
  }
  if (iden_digits < 3) return false;          // 至少 3 位
  {
    const int h4 = hexVal(*p);
    if (h4 >= 0 && (p[1] == '\0' || p[1] == ' ' || p[1] == '\t')) {
      iden = (uint16_t)((iden << 4) | (uint16_t)h4);
      ++p;
      ++iden_digits;
    }
  }

  // 可选 CMD:后面紧跟一个十六进制位、且其后是空白或结束,
  // 才当作 CMD;否则那个字符属于数据(如 "VAN 824 1" 这种奇数位应判错)
  uint8_t cmd = 0x8u;                          // EXT 保留位按约定为 1
  {
    const char* q = skipSpace(p);
    const int h = hexVal(*q);
    const char* r = (h >= 0) ? q + 1 : q;
    const bool followed_by_space_or_end = (*r == '\0' || *r == ' ' || *r == '\t');
    if (h >= 0 && followed_by_space_or_end && iden_digits == 3) {
      // 只有 3 位 iden 时才允许把下一个孤立十六进制位当 CMD,
      // 避免把 4 位 iden 的最后一位吃掉
      cmd = (uint8_t)h;
      p = r;
    }
  }

  // 数据字节:2 位一组
  uint8_t len = 0;
  for (;;) {
    p = skipSpace(p);
    if (!*p) break;
    const int hi = hexVal(p[0]);
    const int lo = (p[1] != '\0') ? hexVal(p[1]) : -1;
    if (hi < 0 || lo < 0) return false;        // 奇数位或非十六进制
    if (len >= sizeof(pkt->data)) return false;
    pkt->data[len++] = (uint8_t)((hi << 4) | lo);
    p += 2;
  }

  pkt->iden = iden;
  pkt->cmd = cmd;
  pkt->ack = 0;          // 回放不模拟总线上其他节点的应答
  pkt->fcs_ok = 1;       // 回放数据视为可信
  pkt->len = len;
  pkt->rx_ms = now_ms;
  return true;
}

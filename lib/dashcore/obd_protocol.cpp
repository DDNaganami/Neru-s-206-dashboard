#include "obd_protocol.h"

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 从 idx 处解析一个十六进制字节(跳过空格),失败返回 -1
static int hexByte(const char* s, uint8_t& idx) {
  while (s[idx] == ' ' || s[idx] == '\t') ++idx;
  const int hi = hexVal(s[idx]);
  const int lo = (s[idx + 1] != '\0') ? hexVal(s[idx + 1]) : -1;
  if (hi < 0 || lo < 0) return -1;
  idx += 2;
  return (hi << 4) | lo;
}

bool parseObdLine(const char* line, uint8_t* pid_out, uint16_t* raw_out) {
  if (!line || !pid_out || !raw_out) return false;

  // 找 "41 0C" / "41 05":ELM327 实际输出各字节间带空格(如 "41 0C 1A F8"),
  // 解析时跳过空格;带 ECU 地址头(如 "8F 41 0C ...")时从 "41" 处命中。
  for (uint8_t i = 0; line[i] != '\0'; ++i) {
    if (line[i] != '4' || line[i + 1] != '1') continue;
    uint8_t j = i + 2;
    while (line[j] == ' ' || line[j] == '\t') ++j;
    if (line[j] != '0') continue;
    const char pc = line[j + 1];
    if (pc != 'C' && pc != '5') continue;
    j += 2;

    const int a = hexByte(line, j);
    if (a < 0) return false;

    if (pc == 'C') {  // 010C:双字节
      const int b = hexByte(line, j);
      if (b < 0) return false;
      *pid_out = 0x0C;
      *raw_out = (uint16_t)((a << 8) | b);
    } else {  // 0105:单字节 A-40
      *pid_out = 0x05;
      *raw_out = (uint16_t)(a & 0xFF);
    }
    return true;
  }
  return false;
}

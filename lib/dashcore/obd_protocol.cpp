#include "obd_protocol.h"

// ============================================================
// 解析层支持的 PID —— **表驱动**
//
// ★ 这里原来是硬编码的两个判断(pc == 'C' || pc == '5'),
//   于是加 010F(进气温度)时解析直接失败:数据喂进来了却"没有进气温度"。
//   而且失败得很安静 —— parseObdLine 返回 false,上层继续等下一帧。
//   改成表驱动之后,以后再加一个"01 服务 + 单字节"的 PID 只需要动这张表。
//
// bytes  = 数据字节数(1 = A-40℃ 那类,2 = (A*256+B)/4 那类)
// ============================================================
struct PidSpec {
  uint8_t pid;
  uint8_t bytes;
};
static const PidSpec kSupportedPids[] = {
  {0x0C, 2},   // 转速  (A*256+B)/4
  {0x05, 1},   // 水温  A-40 ℃
  {0x0F, 1},   // 进气温度 A-40 ℃
  {0x0D, 1},   // 车速  A km/h  ← 2026-09-18 加:是否有这一项由 0100 位图决定,
               //   见 obd_source.cpp 的 buildPollTable(这里是"能不能解析",
               //   那里是"要不要去问"—— 两件事分开,别混成一件)
};
static const uint8_t kSupportedPidCount =
    (uint8_t)(sizeof(kSupportedPids) / sizeof(kSupportedPids[0]));

static uint8_t pidBytes(uint8_t pid) {
  for (uint8_t i = 0; i < kSupportedPidCount; ++i) {
    if (kSupportedPids[i].pid == pid) return kSupportedPids[i].bytes;
  }
  return 0;   // 0 = 不认识(比如 4100 的"支持位图",不该当数据用)
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 从 idx 处解析一个十六进制字节(跳过空格),失败返回 -1
static int hexByte(const char* s, uint8_t& idx) {
  while (s[idx] == ' ' || s[idx] == '\t') ++idx;
  if (s[idx] == '\0') return -1;             // 别读越界(旧版会读 s[idx+1])
  const int hi = hexVal(s[idx]);
  const int lo = hexVal(s[idx + 1]);         // s[idx] 非 0 → s[idx+1] 最多是终止符
  if (hi < 0 || lo < 0) return -1;
  idx += 2;
  return (hi << 4) | lo;
}

bool parseObdLine(const char* line, uint8_t* pid_out, uint16_t* raw_out) {
  if (!line || !pid_out || !raw_out) return false;

  // 找 "41 xx":ELM327 实际输出各字节间带空格(如 "41 0C 1A F8"),
  // 解析时跳过空格;带 ECU 地址头(如 "8F 41 0C ...")时从 "41" 处命中。
  // 逗号/回车等非十六进制字符会被 hexByte 挡掉,所以不需要先做清洗。
  for (uint8_t i = 0; line[i] != '\0'; ++i) {
    if (line[i] != '4' || line[i + 1] != '1') continue;

    uint8_t j = (uint8_t)(i + 2);
    const int pid = hexByte(line, j);
    if (pid < 0) continue;

    const uint8_t nbytes = pidBytes((uint8_t)pid);
    if (nbytes == 0) continue;               // 不是我们轮询的 PID,继续往后找

    const int a = hexByte(line, j);
    if (a < 0) return false;                 // 认出了 PID 却没数据 = 残帧

    if (nbytes == 2) {
      const int b = hexByte(line, j);
      if (b < 0) return false;
      *raw_out = (uint16_t)((a << 8) | b);
    } else {
      *raw_out = (uint16_t)(a & 0xFF);
    }
    *pid_out = (uint8_t)pid;
    return true;
  }
  return false;
}

// ---- 0100 的"支持的 PID 位图" ----
//
// 为什么单独一个函数而不是塞进 parseObdLine:
//   位图是 4 个数据字节,而真实 PID 里最多 2 个 —— 共用一条解析路径就必须
//   在"这是位图还是数据"之间做判断,而两者的判别依据恰好就是 PID 本身。
//   分开写,两边都简单,而且 parseObdLine 的"表驱动"语义不被污染(查表里没有
//   0x00,所以位图天然会被它拒掉)。
bool parseSupportedPids(const char* line, uint32_t* mask_out) {
  if (!line || !mask_out) return false;

  for (uint8_t i = 0; line[i] != '\0'; ++i) {
    if (line[i] != '4' || line[i + 1] != '1') continue;

    uint8_t j = (uint8_t)(i + 2);
    const int pid = hexByte(line, j);
    if (pid != 0x00) continue;               // 只认 0100 那张位图

    uint32_t mask = 0;
    for (int k = 0; k < 4; ++k) {
      const int v = hexByte(line, j);
      if (v < 0) return false;               // 位图不完整 = 残帧,别拿半个去猜
      mask = (mask << 8) | (uint32_t)v;
    }
    *mask_out = mask;
    return true;
  }
  return false;
}

bool pidSupported(uint32_t mask, uint8_t pid) {
  if (pid < 0x01 || pid > 0x20) return false;
  // PID 0x01 = 最高位(bit31)。这条映射写错就会得到一个"看起来合理"的错误答案
  // (比如把 0x0D 判成不支持),所以它有专门的用例钉着:test_obd_supported_pids_bits。
  const uint8_t idx = (uint8_t)(pid - 0x01);   // 0..31
  return (mask & (1UL << (31 - idx))) != 0;
}

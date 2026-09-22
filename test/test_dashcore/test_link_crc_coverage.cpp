// 双板链路 v1 的 CRC-15(0x0F9D)检错覆盖 —— **枚举实测**(用来关掉 ARCHITECTURE.md §8 的 L4)
//
// 这一段回答 §8 那个待确认项:链路帧的 CRC 到底能挡住多少种位错。
//
// 码字模型(与 §8 的「帧格式」「校验」两节一致):
//   链路帧长 = 7 + LEN,LEN ∈ 5..16 ⇒ **12..23 字节 = 96..184 bit**;
//   帧 = 前 L-2 字节(被 CRC 覆盖)+ 末尾 2 字节 **16 位大端字段**,
//   字段里装 15 位 CRC(bit15 恒 0)⇒ 判据就是"16 位整数相等":
//       crc15_van_iso(前 L-2 字节) == (f[L-2] << 8 | f[L-1])
//   ★ 一处口径必须说清楚,免得把结论说过头:§8 的真实帧第一个字节是 SYNC,
//     而 **SYNC 不进 CRC 覆盖**。本枚举按"码字 = 整帧"建模型(前 L-2 字节整体
//     当数据)。真链路上 SYNC 单独错的那种帧**走不到 CRC 这一关** —— 帧头找不到
//     SYNC,按 §「重同步」丢掉并计进另一路计数。所以下面这些数不能说成
//     "SYNC 位错由 CRC 检出"。
//
// 怎么做的、凭什么信(三层证据,逐条是可复跑的用例):
//   · 每个帧长先造一个合法合成帧(数据用确定性伪随机,末尾两字节填自己算的 CRC),
//     然后对**每一个**错误图案判定"未检出"。
//   · 判定不逐图案重算 CRC,而用 CRC 的**仿射线性**(省下 5×10^7 次全帧 CRC):
//       delta[p] = 翻转第 p 位后判据值 ^ 原判据值(与帧内容无关,每个帧长一张表)
//       ⇒ 未检出 ⟺ 图案各位 delta 的异或 = 0。
//     这条假设由两个独立检验兜着:
//       ① 换一帧重算 delta,必须逐位相同(model_selfcheck);
//       ② 抽样的 3 位错与突发用**真实 crc15_van_iso** 逐条复核,必须一条不差;
//          1 位/2 位错更是**全量**用真实 CRC 复核(1bit_2bit_exhaustive)。
//   · 计数是**枚举**出来的(不是抽样、也不是推导):1 位 / 2 位 / 3 位 /
//     长度 2..16 的全部突发,一个不落。理论保证另外单独写成断言。
//
// 跑法:python -m platformio test -e native(宿主机,纯 ASCII 副本);
//   **要看表格得加 -v** —— PlatformIO 只转发"用例结果行",未解析的 printf 行默认被丢掉。
//   表格和本文件里所有 printf 都是纯 ASCII,原因见 printTable() 上面的注释。
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "van_wire.h"

using namespace van;

namespace {

const uint8_t  kLenMin  = 12;                        // v1:7 + LEN,LEN = 5..16
const uint8_t  kLenMax  = 23;
const uint8_t  kRowN    = (uint8_t)(kLenMax - kLenMin + 1);
const uint16_t kBitsMax = (uint16_t)(kLenMax * 8);   // 184
const uint32_t kXorMax  = 65536u;                    // delta 是 16 位:翻转那个"恒 0"位给 0x8000

// ---------- 判据(照 §8 的字段表写;不依赖编码器/解析器) ----------
inline uint16_t linkSyndrome(const uint8_t* f, uint8_t len) {
  const uint16_t field = (uint16_t)(((uint16_t)f[len - 2] << 8) | f[len - 1]);
  return (uint16_t)(crc15_van_iso(f, (uint16_t)(len - 2)) ^ field);
}
inline bool linkFrameLooksGood(const uint8_t* f, uint8_t len) {
  return linkSyndrome(f, len) == 0u;
}
inline void flipBit(uint8_t* f, uint16_t pos) {
  f[pos >> 3] ^= (uint8_t)(0x80u >> (pos & 7));
}
inline uint32_t frameSeed(uint8_t len) {
  return 0x9E3779B9u * (uint32_t)len + 0x1234u;
}

// 造一个合法帧:数据用确定性伪随机(⇒ 不依赖某个具体报文内容),
// 末尾两字节填自己算出来的 CRC(高字节先发,bit15 恒 0)。
void buildFrame(uint8_t len, uint8_t* f) {
  uint32_t seed = frameSeed(len);
  for (uint8_t i = 0; i + 2 < len; ++i) {
    seed = seed * 1103515245u + 12345u;
    f[i] = (uint8_t)(seed >> 16);
  }
  const uint16_t crc = crc15_van_iso(f, (uint16_t)(len - 2));
  f[len - 2] = (uint8_t)(crc >> 8);
  f[len - 1] = (uint8_t)(crc & 0xFF);
}

struct Row {
  uint8_t  len       = 0;
  uint16_t bits      = 0;
  uint32_t undet1    = 0;    // 1 位错未检出
  uint32_t undet2    = 0;    // 2 位错未检出
  uint32_t undet3    = 0;    // 3 位错未检出
  uint64_t burstLe15 = 0;    // 长度 2..15 bit 的突发未检出
  uint64_t burst16   = 0;    // 长度 16 bit 的突发未检出
  uint64_t burstsAll = 0;    // 枚举过的突发总数(2..16)
  bool     patOk     = false;   // 16 位突发的未检出图案:首/末起点是同一个
  uint16_t pat       = 0;
  bool     wit4      = false;   // 找到 4 位错的未检出见证
  uint16_t wit[4]    = {0, 0, 0, 0};
  uint16_t delta[kBitsMax] = {0};
};

struct Coverage {
  bool     done = false;
  Row      row[kRowN];
  uint64_t undet1 = 0, undet2 = 0, undet3 = 0, burstLe15 = 0, burst16 = 0, burstsAll = 0;
  uint64_t samples = 0;         // 真实 CRC 抽样复核次数
  uint32_t mismatch = 0;        // 抽样里"线性表 vs 真实 CRC"不一致的次数(必须 0)
  uint32_t burstLe15Leak = 0;   // 抽样里"长度 ≤15 的突发被真实 CRC 放过"的次数(必须 0)
  uint32_t deltaDiffs = 0;      // 换一帧后 delta 不同的位数(必须 0)
  uint32_t selfFail = 0;        // 合成帧自己都没过判据的帧长数(必须 0)
  uint8_t  minDist = 0;         // 实测最小汉明距离
};

Coverage g_cov;

// 长度 b 的全部突发:图案 = 首末两位必为 1、中间 b-2 位任意。
// 某个起点 s 上的判据 = delta[s] ^ delta[s+b-1] ^ (中间置位那几位的 delta 异或);
// 中间那 2^(b-2) 个图案按格雷码走(相邻图案只差一位)⇒ 每个图案 O(1)。
uint64_t countBursts(const uint16_t* delta, uint16_t bits, uint8_t b) {
  uint64_t bad = 0;
  const uint32_t mid = 1u << (b - 2);
  for (uint16_t s = 0; (uint16_t)(s + b) <= bits; ++s) {
    uint16_t acc = (uint16_t)(delta[s] ^ delta[s + b - 1]);
    if (acc == 0u) ++bad;                       // 中间全 0 的那个图案
    uint32_t prev = 0;
    for (uint32_t m = 1; m < mid; ++m) {
      const uint32_t gray = m ^ (m >> 1);
      uint32_t diff = gray ^ prev;
      uint8_t bit = 0;
      while ((diff & 1u) == 0u) { diff >>= 1; ++bit; }
      acc ^= delta[s + 1 + bit];
      prev = gray;
      if (acc == 0u) ++bad;
    }
  }
  return bad;
}

// 找某个起点上未检出的 16 位突发图案(返回 false = 这个起点一个都没有)。
// 只给"图案是不是同一个"这条用,所以直接暴力枚举 2^14 个中间图案。
bool findBurst16Pattern(const uint16_t* delta, uint16_t s, uint16_t* pat) {
  for (uint32_t m = 0; m < (1u << 14); ++m) {
    uint16_t acc = (uint16_t)(delta[s] ^ delta[s + 15]);
    for (uint8_t t = 0; t < 14; ++t) {
      if (m & (1u << t)) acc ^= delta[s + 1 + t];
    }
    if (acc == 0u) {
      *pat = (uint16_t)(0x8000u | (m << 1) | 1u);   // bit15 = 起点那位,bit0 = 末位
      return true;
    }
  }
  return false;
}

// 最小汉明距离的见证:两个**不相交**的位对异或相同 ⇒ 这 4 位全翻、判据不变。
// (1/2/3 位错全为 0 已由前面几条钉住,所以找到 4 位错见证就等于 d = 4。)
bool findWeight4Witness(const uint16_t* delta, uint16_t bits, uint16_t* out) {
  static int32_t firstA[kXorMax];
  static int32_t firstB[kXorMax];
  for (uint32_t x = 0; x < kXorMax; ++x) { firstA[x] = -1; firstB[x] = -1; }
  for (uint16_t i = 0; i < bits; ++i) {
    for (uint16_t j = (uint16_t)(i + 1); j < bits; ++j) {
      const uint32_t x = (uint32_t)(delta[i] ^ delta[j]);
      if (firstA[x] < 0) {
        firstA[x] = i;
        firstB[x] = j;
      } else if (firstA[x] != i && firstA[x] != j && firstB[x] != i && firstB[x] != j) {
        out[0] = (uint16_t)firstA[x];
        out[1] = (uint16_t)firstB[x];
        out[2] = i;
        out[3] = j;
        return true;
      }
    }
  }
  return false;
}

// ★ 表格(以及本文件里所有 printf)一律**纯 ASCII**:
//   PlatformIO 默认只转发"用例结果行",未解析的行只在 -v 下 echo;而 echo 要走
//   中文 Windows 的 GBK 控制台 —— 中文 printf 会抛 UnicodeEncodeError,
//   把 -v 那次跑的用例统计一起打乱(实测 128 例被打成 124 例)。
//   所以:报告用 ASCII,断言消息里的中文照旧(那种行走的是转义路径,没事)。
void printTable() {
  printf("\nlink CRC-15(0x0F9D) error-detection coverage -- frame len 12..23 B (96..184 bit codeword)\n");
  printf("len_B  bits  err1  err2  err3  burst_2..15  burst_16  b16_pattern  min_dist\n");
  for (uint8_t r = 0; r < kRowN; ++r) {
    const Row& row = g_cov.row[r];
    printf("%5u %5u %5u %5u %5u %12llu %9llu      0x%04X %9u\n",
           (unsigned)row.len, (unsigned)row.bits, (unsigned)row.undet1,
           (unsigned)row.undet2, (unsigned)row.undet3,
           (unsigned long long)row.burstLe15, (unsigned long long)row.burst16,
           (unsigned)row.pat, (unsigned)row.wit4 ? 4u : 0u);
  }
  printf("TOTAL       %5llu %5llu %5llu %12llu %9llu   burst patterns enumerated: %llu\n",
         (unsigned long long)g_cov.undet1, (unsigned long long)g_cov.undet2,
         (unsigned long long)g_cov.undet3, (unsigned long long)g_cov.burstLe15,
         (unsigned long long)g_cov.burst16, (unsigned long long)g_cov.burstsAll);
  printf("theory: err1/err2 = 0; err3 = 0 (odd weight -- g contains (x+1)); burst <= 15 bit always\n");
  printf("        detected; burst_16 has no closed-form guarantee (measured value above)\n");
  printf("cross-check with real crc15_van_iso: %llu sampled patterns, %u mismatches\n",
         (unsigned long long)g_cov.samples, (unsigned)g_cov.mismatch);
  printf("(run with -v to see this table; PlatformIO drops unparsed lines without it)\n\n");
  // ★ 必须自己 flush:测试进程在别的用例里有个**既有崩溃**(exit 3),
  //   走到那里时缓冲里的表格会被整段丢掉(实测:不加这句,一行都没有)。
  fflush(stdout);
}

// 全量枚举一次(约 5.6×10^7 个图案判定),结果缓存;表格也在这里打一次。
void computeCoverage() {
  if (g_cov.done) return;

  for (uint8_t r = 0; r < kRowN; ++r) {
    Row& row = g_cov.row[r];
    row.len  = (uint8_t)(kLenMin + r);
    row.bits = (uint16_t)(row.len * 8u);

    uint8_t f[kLenMax] = {0};
    buildFrame(row.len, f);
    if (!linkFrameLooksGood(f, row.len)) ++g_cov.selfFail;

    // delta 表:翻转第 p 位对判据值的贡献
    const uint16_t base = linkSyndrome(f, row.len);
    for (uint16_t p = 0; p < row.bits; ++p) {
      flipBit(f, p);
      row.delta[p] = (uint16_t)(linkSyndrome(f, row.len) ^ base);
      flipBit(f, p);                                  // 异或自反,翻回来
    }
    // ① delta 与帧内容无关(线性性):换一帧必须逐位相同
    uint8_t f2[kLenMax] = {0};
    buildFrame(row.len, f2);
    f2[0] ^= 0x5Au;                                   // 再换几个字节,确保不是同一个帧
    f2[1] ^= 0x3Cu;
    const uint16_t crc2 = crc15_van_iso(f2, (uint16_t)(row.len - 2));
    f2[row.len - 2] = (uint8_t)(crc2 >> 8);
    f2[row.len - 1] = (uint8_t)(crc2 & 0xFF);
    const uint16_t base2 = linkSyndrome(f2, row.len);
    for (uint16_t p = 0; p < row.bits; ++p) {
      flipBit(f2, p);
      if ((uint16_t)(linkSyndrome(f2, row.len) ^ base2) != row.delta[p]) ++g_cov.deltaDiffs;
      flipBit(f2, p);
    }

    // ② 枚举 1/2/3 位错
    for (uint16_t i = 0; i < row.bits; ++i) {
      if (row.delta[i] == 0u) ++row.undet1;
      for (uint16_t j = (uint16_t)(i + 1); j < row.bits; ++j) {
        const uint16_t dij = (uint16_t)(row.delta[i] ^ row.delta[j]);
        if (dij == 0u) ++row.undet2;
        for (uint16_t k = (uint16_t)(j + 1); k < row.bits; ++k) {
          if ((uint16_t)(dij ^ row.delta[k]) == 0u) ++row.undet3;
        }
      }
    }

    // ③ 枚举全部突发:长度 2..16
    for (uint8_t b = 2; b <= 16; ++b) {
      const uint64_t bad = countBursts(row.delta, row.bits, b);
      row.burstsAll += (uint64_t)(row.bits - b + 1) * (uint64_t)(1u << (b - 2));
      if (b <= 15) row.burstLe15 += bad;
      else         row.burst16 = bad;
    }
    // 16 位突发:首/末那个"数据区内"的起点上,未检出图案是不是同一个
    {
      const uint16_t firstS = 0;
      const uint16_t lastS  = (uint16_t)(8u * (row.len - 2u) - 16u);
      uint16_t p1 = 0, p2 = 0;
      const bool ok1 = findBurst16Pattern(row.delta, firstS, &p1);
      const bool ok2 = findBurst16Pattern(row.delta, lastS, &p2);
      row.patOk = ok1 && ok2 && (p1 == p2);
      row.pat   = p1;
    }

    // ④ 4 位错见证(= 最小汉明距离 4 的证据)
    row.wit4 = findWeight4Witness(row.delta, row.bits, row.wit);

    // ⑤ 抽样:线性表说的必须和真实 crc15_van_iso 的判定逐条一致
    uint32_t rng = 0x2468ACE1u ^ (uint32_t)row.len;
    for (int t = 0; t < 600; ++t) {
      uint16_t pos[3];
      for (int q = 0; q < 3; ++q) {
        rng = rng * 1103515245u + 12345u;
        pos[q] = (uint16_t)((rng >> 8) % row.bits);
        for (int q2 = 0; q2 < q; ++q2) {          // 去重,免得退化成 1 位错
          while (pos[q] == pos[q2]) {
            rng = rng * 1103515245u + 12345u;
            pos[q] = (uint16_t)((rng >> 8) % row.bits);
          }
        }
      }
      const bool pred = (uint16_t)(row.delta[pos[0]] ^ row.delta[pos[1]] ^ row.delta[pos[2]]) == 0u;
      uint8_t g[kLenMax];
      memcpy(g, f, sizeof(g));
      flipBit(g, pos[0]); flipBit(g, pos[1]); flipBit(g, pos[2]);
      const bool act = linkFrameLooksGood(g, row.len);
      ++g_cov.samples;
      if (pred != act) ++g_cov.mismatch;
    }
    for (int t = 0; t < 600; ++t) {
      rng = rng * 1103515245u + 12345u;
      const uint8_t b = (uint8_t)(2u + ((rng >> 8) % 15u));          // 2..16
      rng = rng * 1103515245u + 12345u;
      const uint16_t s = (uint16_t)((rng >> 8) % (uint16_t)(row.bits - b + 1u));
      rng = rng * 1103515245u + 12345u;
      const uint32_t m = (rng >> 8) & ((1u << (b - 2)) - 1u);        // 中间 b-2 位
      uint16_t acc = (uint16_t)(row.delta[s] ^ row.delta[s + b - 1]);
      for (uint8_t q = 0; q < (uint8_t)(b - 2); ++q) {
        if (m & (1u << q)) acc ^= row.delta[s + 1 + q];
      }
      const bool pred = (acc == 0u);
      uint8_t g[kLenMax];
      memcpy(g, f, sizeof(g));
      flipBit(g, s);
      flipBit(g, (uint16_t)(s + b - 1));
      for (uint8_t q = 0; q < (uint8_t)(b - 2); ++q) {
        if (m & (1u << q)) flipBit(g, (uint16_t)(s + 1 + q));
      }
      const bool act = linkFrameLooksGood(g, row.len);
      ++g_cov.samples;
      if (pred != act) ++g_cov.mismatch;
      if (b <= 15u && act) ++g_cov.burstLe15Leak;   // 长度 ≤15 被真实 CRC 放过的次数(必须 0)
    }

    g_cov.undet1    += row.undet1;
    g_cov.undet2    += row.undet2;
    g_cov.undet3    += row.undet3;
    g_cov.burstLe15 += row.burstLe15;
    g_cov.burst16   += row.burst16;
    g_cov.burstsAll += row.burstsAll;
  }
  g_cov.minDist = (g_cov.undet1 + g_cov.undet2 + g_cov.undet3 == 0u) ? 4u : 3u;
  printTable();
  g_cov.done = true;
}

const Coverage& coverage() {
  computeCoverage();
  return g_cov;
}

}  // namespace

// ---------- 用例 ----------

// 模型与线性性自检:合成帧必须过判据;delta 表必须与帧内容无关;
// 抽样的 3 位错/突发必须与真实 crc15_van_iso 判定逐条一致。
// 这条不通过,下面所有计数都不能信。
static void test_link_crc_model_selfcheck(void) {
  const Coverage& c = coverage();
  char msg[160];
  snprintf(msg, sizeof(msg), "有 %u 个帧长的合成帧没过自己的 CRC 判据", (unsigned)c.selfFail);
  TEST_ASSERT_EQUAL_MESSAGE(0u, c.selfFail, msg);
  snprintf(msg, sizeof(msg), "换一帧后 delta 有 %u 位不同(线性性不成立?)", (unsigned)c.deltaDiffs);
  TEST_ASSERT_EQUAL_MESSAGE(0u, c.deltaDiffs, msg);
  snprintf(msg, sizeof(msg), "真实 CRC 抽样复核 %llu 次:线性表与实现不一致 %u 次",
           (unsigned long long)c.samples, (unsigned)c.mismatch);
  TEST_ASSERT_EQUAL_MESSAGE(0u, c.mismatch, msg);
  snprintf(msg, sizeof(msg), "抽样里长度 ≤15 bit 的突发被真实 CRC 放过 %u 次",
           (unsigned)c.burstLe15Leak);
  TEST_ASSERT_EQUAL_MESSAGE(0u, c.burstLe15Leak, msg);
  printf("selfcheck: %u frame lengths ok, delta identical across frames, "
         "%llu real-CRC samples all consistent\n",
         (unsigned)kRowN, (unsigned long long)c.samples);
  fflush(stdout);
}

// 全部 1 位错与 2 位错:**全量**用真实 crc15_van_iso 复核(不用线性表)⇒ 未检出必须为 0。
static void test_link_crc_1bit_2bit_exhaustive(void) {
  const Coverage& c = coverage();
  uint32_t bad1 = 0, bad2 = 0;
  char msg[160];
  for (uint8_t r = 0; r < kRowN; ++r) {
    const Row& row = c.row[r];
    uint8_t f[kLenMax];
    buildFrame(row.len, f);
    TEST_ASSERT_TRUE_MESSAGE(linkFrameLooksGood(f, row.len), "合成帧没过判据");
    uint32_t r1 = 0, r2 = 0;
    for (uint16_t i = 0; i < row.bits; ++i) {
      flipBit(f, i);
      if (linkFrameLooksGood(f, row.len)) ++r1;
      for (uint16_t j = (uint16_t)(i + 1); j < row.bits; ++j) {
        flipBit(f, j);
        if (linkFrameLooksGood(f, row.len)) ++r2;
        flipBit(f, j);
      }
      flipBit(f, i);
    }
    snprintf(msg, sizeof(msg), "帧长 %u B(%u bit):真实 CRC 全量枚举出 1 位错未检出 %u 个 / 2 位错 %u 个",
             (unsigned)row.len, (unsigned)row.bits, (unsigned)r1, (unsigned)r2);
    TEST_ASSERT_EQUAL_MESSAGE(0u, r1, msg);
    TEST_ASSERT_EQUAL_MESSAGE(0u, r2, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r1, row.undet1, "真实 CRC 与枚举表的 1 位错计数不一致");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(r2, row.undet2, "真实 CRC 与枚举表的 2 位错计数不一致");
    bad1 += r1;
    bad2 += r2;
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, bad1, "存在未检出的 1 位错");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, bad2, "存在未检出的 2 位错");
}

// 全部 3 位错:未检出必须为 0 —— 这条是**理论保证**,不是"碰巧为 0":
// 该多项式有 10 个非零项(偶数)⇒ 含 (x+1) 因子 ⇒ 一切奇数位错(含 3 位错)必被检出。
// (原先 §8 里"3 位错会非零"的猜测不成立;线性表全量枚举 + 真实 CRC 抽样复核都是 0。)
static void test_link_crc_3bit_exhaustive(void) {
  const Coverage& c = coverage();
  char msg[160];
  for (uint8_t r = 0; r < kRowN; ++r) {
    const Row& row = c.row[r];
    snprintf(msg, sizeof(msg),
             "帧长 %u B(%u bit):3 位错未检出 %u 个(理论应为 0:奇数位错 + g 含 (x+1) 因子)",
             (unsigned)row.len, (unsigned)row.bits, (unsigned)row.undet3);
    TEST_ASSERT_EQUAL_MESSAGE(0u, row.undet3, msg);
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, (uint32_t)c.undet3, "3 位错未检出总数不为 0");
}

// 全部突发(长度 2..16 bit):
//   · 长度 ≤ 15:理论保证必检出 ⇒ 断言 0;
//   · 长度 16   :没有闭式保证 ⇒ 断言"实测出来的结构"(见下),并把数字打出来。
// 实测结构:每个帧长恰好 8(L-2)-15 个未检出(65/73/…/153,合计 1308),
//   全部落在**被 CRC 覆盖的数据区**内、每个起点恰好 1 个图案(首末起点图案相同);
//   凡是碰到末尾 CRC 字段的 16 位突发,一个都没被放过。
static void test_link_crc_burst_coverage(void) {
  const Coverage& c = coverage();
  char msg[200];
  for (uint8_t r = 0; r < kRowN; ++r) {
    const Row& row = c.row[r];
    snprintf(msg, sizeof(msg),
             "帧长 %u B(%u bit):长度 2..15 的突发未检出 %llu 个(理论应为 0)",
             (unsigned)row.len, (unsigned)row.bits, (unsigned long long)row.burstLe15);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, (uint32_t)row.burstLe15, msg);
    const uint64_t want = (uint64_t)(8u * (row.len - 2u) - 15u);
    snprintf(msg, sizeof(msg),
             "帧长 %u B:16 位突发未检出 %llu 个,实测结构为 8(L-2)-15 = %llu",
             (unsigned)row.len, (unsigned long long)row.burst16, (unsigned long long)want);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)want, (uint32_t)row.burst16, msg);
    snprintf(msg, sizeof(msg), "帧长 %u B:16 位突发的未检出图案在首/末起点上不一致(pat=0x%04X)",
             (unsigned)row.len, (unsigned)row.pat);
    TEST_ASSERT_TRUE_MESSAGE(row.patOk, msg);
    // 实测基线:图案恒为 0xB9F1(与起点、帧长、帧内容都无关);顺带一提,
    // 它正好是 0x8F9D 的位反转 —— 记下来,改多项式的人会先撞到这一条。
    snprintf(msg, sizeof(msg), "帧长 %u B:16 位突发的未检出图案 = 0x%04X,实测基线是 0xB9F1",
             (unsigned)row.len, (unsigned)row.pat);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xB9F1u, row.pat, msg);
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, (uint32_t)c.burstLe15, "长度 ≤15 的突发有未检出的");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1308u, (uint32_t)c.burst16,
                                   "16 位突发未检出总数与本轮实测基线(1308)不一致");
}

// 实测最小汉明距离 = 4:
//   ≤3 位错的未检出都是 0(上面几条)⇒ d ≥ 4;(x+1) 因子还排除一切奇数权重 ⇒ d 是偶数;
//   枚举找到一个**用真实 CRC 复核过**的 4 位错见证 ⇒ d = 4(每个帧长都是)。
static void test_link_crc_min_distance(void) {
  const Coverage& c = coverage();
  char msg[200];
  for (uint8_t r = 0; r < kRowN; ++r) {
    const Row& row = c.row[r];
    snprintf(msg, sizeof(msg), "帧长 %u B:没有找到 4 位错见证", (unsigned)row.len);
    TEST_ASSERT_TRUE_MESSAGE(row.wit4, msg);

    uint8_t f[kLenMax];
    buildFrame(row.len, f);
    for (int q = 0; q < 4; ++q) flipBit(f, row.wit[q]);
    snprintf(msg, sizeof(msg), "帧长 %u B:4 位错见证(位 %u/%u/%u/%u)没能骗过 CRC ⇒ 它不是见证",
             (unsigned)row.len, (unsigned)row.wit[0], (unsigned)row.wit[1],
             (unsigned)row.wit[2], (unsigned)row.wit[3]);
    TEST_ASSERT_TRUE_MESSAGE(linkFrameLooksGood(f, row.len), msg);
    printf("len %u B: min Hamming distance 4 (witness bits %u/%u/%u/%u)\n",
           (unsigned)row.len, (unsigned)row.wit[0], (unsigned)row.wit[1],
           (unsigned)row.wit[2], (unsigned)row.wit[3]);
    fflush(stdout);   // 同 printTable():进程可能崩在后面的用例里,别让缓冲吃掉报告
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(4u, c.minDist, "实测最小汉明距离不是 4");
}

void register_link_crc_coverage_tests(void) {
  RUN_TEST(test_link_crc_model_selfcheck);
  RUN_TEST(test_link_crc_1bit_2bit_exhaustive);
  RUN_TEST(test_link_crc_3bit_exhaustive);
  RUN_TEST(test_link_crc_burst_coverage);
  RUN_TEST(test_link_crc_min_distance);
}

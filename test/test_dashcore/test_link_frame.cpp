// 双板链路协议 v1 —— **帧层**用例（契约：ARCHITECTURE.md §2「帧格式」）
//
// 这一组只谈帧：字段布局、帧长 = 7 + LEN、CRC 的覆盖范围与字段形态、
// 每一条拒绝路径、以及 §2 那条"版本不匹配 ≠ 丢帧"的规矩。
// 消息语义在 test_link_msg.cpp，字节流分帧/重同步在 test_link_phy.cpp。
//
// ★ CRC 的"能力"（1/2/3 位错、突发、最小汉明距离）**不在这里重测** ——
//   那是 test_link_crc_coverage.cpp 的枚举结论（§8 的 L4）。这里测的是
//   "**这一层**有没有按契约把 CRC 摆对位置"：覆盖 VER..载荷、不含 SYNC、
//   字段是 15 位直存（不是 VAN 那套左移）。
#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "link_frame.h"
#include "link_msg.h"    // 载荷长度（kTickLen / kDataLen / …）也要跟帧长对账
#include "link_role.h"
#include "van_wire.h"   // 只为对照：van::crc15_van_iso / fcsFieldFromCrc（反例用）

using namespace dashlink;

namespace {

// 解码结果按名字比 —— 失败时消息里直接写清"期望 crc、实际 bad_sync"，
// 比打印枚举数字有用得多（枚举号是内部实现，名字才是契约里的说法）。
void assertDecode(DecodeErr want, DecodeErr got, const char* msg) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE(decodeErrName(want), decodeErrName(got), msg);
}

void fillPayload(uint8_t* p, uint8_t len, uint8_t seed) {
  for (uint8_t i = 0; i < len; ++i) p[i] = (uint8_t)(seed + i * 7u);
}

// 按 §2 的偏移表**手写**一帧（不经过 encodeFrame）。用来造 encodeFrame
// 明确拒收的那种帧（LEN 越界），或者造"字段被人按别的约定摆过"的帧。
uint16_t buildFrameByHand(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t role,
                          uint8_t ver, uint8_t* out) {
  out[kOffSync] = kSync;
  out[kOffVer]  = ver;
  out[kOffType] = type;
  out[kOffLen]  = len;
  out[kOffRole] = role;
  for (uint8_t i = 0; i < len; ++i) out[kOffPayload + i] = payload[i];
  const uint16_t n = frameBytesForLen(len);
  const uint16_t crc = frameCrc(out, n);
  out[n - 2u] = (uint8_t)(crc >> 8);
  out[n - 1u] = (uint8_t)(crc & 0xFFu);
  return n;
}

}  // namespace

// ------------------------------------------------------------
// 字段布局 + 往返：逐字节对 §2 的偏移表
// ------------------------------------------------------------
static void test_link_frame_layout_and_roundtrip(void) {
  uint8_t payload[8];
  fillPayload(payload, 8, 0x11);
  uint8_t buf[kParseBufBytes];
  const uint16_t n = encodeFrame((uint8_t)MsgType::Data, payload, 8, kRoleMaster, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT16(15u, n);                       // 帧长 = 7 + LEN
  TEST_ASSERT_EQUAL_HEX8(kSync, buf[kOffSync]);
  TEST_ASSERT_EQUAL_HEX8(kVer, buf[kOffVer]);             // v1 = 0x10
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Data, buf[kOffType]);
  TEST_ASSERT_EQUAL_UINT8(8u, buf[kOffLen]);
  TEST_ASSERT_EQUAL_UINT8(kRoleMaster, buf[kOffRole]);
  for (uint8_t i = 0; i < 8; ++i) {
    TEST_ASSERT_EQUAL_HEX8(payload[i], buf[kOffPayload + i]);
  }
  // CRC 覆盖 VER..载荷末尾 = 字节 1..(4+LEN) = 12 个字节
  TEST_ASSERT_EQUAL_HEX16(van::crc15_van_iso(buf + kOffVer, 12u),
                          (uint16_t)(((uint16_t)buf[13] << 8) | buf[14]));

  Frame f;
  assertDecode(DecodeErr::Ok, decodeFrame(buf, n, &f), "自造的帧必须能解回来");
  TEST_ASSERT_EQUAL_HEX8(kVer, f.ver);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)MsgType::Data, f.type);
  TEST_ASSERT_EQUAL_UINT8(8u, f.len);
  TEST_ASSERT_EQUAL_UINT8(kRoleMaster, f.role);
  TEST_ASSERT_FALSE(f.ver_mismatch);
  TEST_ASSERT_EQUAL_HEX16(f.crc, f.crc_calc);
  for (uint8_t i = 0; i < 8; ++i) {
    TEST_ASSERT_EQUAL_HEX8(payload[i], f.payload[i]);
  }
}

// LEN 的每一个合法值都要能往返（v1 合法范围 5..16，两个端点各是一条边界）
static void test_link_frame_len_bounds_roundtrip(void) {
  uint8_t payload[kLenMax];
  fillPayload(payload, kLenMax, 0x30);
  for (uint8_t len = kLenMin; len <= kLenMax; ++len) {
    uint8_t buf[kParseBufBytes];
    const uint16_t n = encodeFrame((uint8_t)MsgType::Status, payload, len, kRoleSlave,
                                   buf, sizeof(buf));
    char msg[96];
    snprintf(msg, sizeof(msg), "LEN = %u 编码失败(帧长应为 %u)", (unsigned)len,
             (unsigned)frameBytesForLen(len));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(frameBytesForLen(len), n, msg);
    Frame f;
    snprintf(msg, sizeof(msg), "LEN = %u 的帧解不回来", (unsigned)len);
    assertDecode(DecodeErr::Ok, decodeFrame(buf, n, &f), msg);
    TEST_ASSERT_EQUAL_UINT8(len, f.len);
    for (uint8_t i = 0; i < len; ++i) {
      snprintf(msg, sizeof(msg), "LEN = %u 载荷第 %u 字节对不上", (unsigned)len, (unsigned)i);
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(payload[i], f.payload[i], msg);
    }
  }
}

// ------------------------------------------------------------
// CRC 覆盖范围：覆盖区里**每一位**单错都必须被 CRC 抓住；
// SYNC 那 8 位单错**一个都不归 CRC 管**（由 BadSync 兜）—— 这条正是 §2
// "SYNC 刻意不进 CRC"的钉子，也是"改 SYNC 不影响 CRC 校验结果"的行为。
// ------------------------------------------------------------
static void test_link_frame_crc_covers_ver_to_payload(void) {
  uint8_t payload[kDataLen];
  fillPayload(payload, kDataLen, 0x5A);
  uint8_t buf[kParseBufBytes];
  const uint16_t n = encodeFrame((uint8_t)MsgType::Data, payload, kDataLen, kRoleMaster,
                                 buf, sizeof(buf));
  const uint16_t base_crc = frameCrc(buf, n);
  const uint16_t covered_end = (uint16_t)(kOffPayload + kDataLen - 1u);   // 4 + LEN

  for (uint16_t pos = 0; pos < n; ++pos) {
    for (uint8_t bit = 0; bit < 8; ++bit) {
      uint8_t g[kParseBufBytes];
      memcpy(g, buf, n);
      g[pos] = (uint8_t)(g[pos] ^ (uint8_t)(1u << bit));

      Frame f;
      const DecodeErr e = decodeFrame(g, n, &f);
      char msg[128];
      snprintf(msg, sizeof(msg), "改了字节 %u 的第 %u 位:解码结果是 %s",
               (unsigned)pos, (unsigned)bit, decodeErrName(e));
      if (pos == kOffSync) {
        assertDecode(DecodeErr::BadSync, e, msg);
        // ★ 关键的一条：SYNC 改动**完全不进** CRC 判据
        snprintf(msg, sizeof(msg), "改 SYNC 第 %u 位后 frameCrc 变了 ⇒ SYNC 被算进覆盖范围了",
                 (unsigned)bit);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(base_crc, frameCrc(g, n), msg);
      } else if (pos == kOffLen) {
        // ★ LEN 这个字节特殊：改它 = 改帧长，判据顺序里 ③/④ 排在 CRC 前面
        //   （见 decodeFrame）。所以期望不是 crc 而是：
        //     · 新的 LEN > 64（bit6/bit7 被翻起来）⇒ 不等载荷，立刻 len_range
        //     · 否则 n 与新 LEN 对不上            ⇒ len_mismatch
        const uint8_t newLen = g[kOffLen];
        if (newLen > kLenNoWaitAbove) {
          snprintf(msg, sizeof(msg), "LEN 改成 %u(>%u)必须立刻判越界", (unsigned)newLen,
                   (unsigned)kLenNoWaitAbove);
          assertDecode(DecodeErr::LenOutOfRange, e, msg);
        } else {
          snprintf(msg, sizeof(msg), "LEN 改成 %u 之后帧长对不上(n 仍是 %u)", (unsigned)newLen,
                   (unsigned)n);
          assertDecode(DecodeErr::LenMismatch, e, msg);
        }
      } else if (pos + kCrcBytes >= n) {
        // 末尾 2 字节 = **CRC 字段自己**：同样不进覆盖范围（frameCrc 不动），
        // 但字段被改了一位就必然对不上 ⇒ 还是 crc 错。
        assertDecode(DecodeErr::CrcError, e, msg);
        snprintf(msg, sizeof(msg), "改了 CRC 字段自己的字节 %u 第 %u 位后 frameCrc 变了 ⇒ "
                                   "CRC 字段被算进覆盖范围了", (unsigned)pos, (unsigned)bit);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(base_crc, frameCrc(g, n), msg);
      } else {
        assertDecode(DecodeErr::CrcError, e, msg);
        // 覆盖区(1..4+LEN)的单错必然改变 CRC
        snprintf(msg, sizeof(msg), "改了字节 %u 第 %u 位后 frameCrc 没变(不应发生在覆盖区)",
                 (unsigned)pos, (unsigned)bit);
        TEST_ASSERT_TRUE_MESSAGE(frameCrc(g, n) != base_crc, msg);
      }
    }
  }
  // 顺带把"覆盖区到哪结束"写下来：4 + LEN 是最后一个被覆盖的字节
  TEST_ASSERT_EQUAL_UINT16((uint16_t)(kOffRole + kDataLen), covered_end);
}

// "改 SYNC 不影响 CRC 校验结果"单独再钉一次（不靠上一条的循环读起来更直白）：
// 8 种单错 + 一个完全换掉的字节，CRC 判据都不动，唯一变化的是 BadSync。
static void test_link_frame_sync_outside_crc(void) {
  uint8_t payload[kTickLen];
  fillPayload(payload, kTickLen, 0x0F);
  uint8_t buf[kParseBufBytes];
  const uint16_t n = encodeFrame((uint8_t)MsgType::Tick, payload, kTickLen, kRoleMaster,
                                 buf, sizeof(buf));
  const uint16_t field = (uint16_t)(((uint16_t)buf[n - 2u] << 8) | buf[n - 1u]);
  TEST_ASSERT_EQUAL_HEX16(field, frameCrc(buf, n));

  for (uint16_t v = 0; v < 256; ++v) {
    if (v == (uint16_t)kSync) continue;
    uint8_t g[kParseBufBytes];
    memcpy(g, buf, n);
    g[kOffSync] = (uint8_t)v;
    Frame f;
    const DecodeErr e = decodeFrame(g, n, &f);
    char msg[96];
    snprintf(msg, sizeof(msg), "SYNC 换成 0x%02X 之后:期望 bad_sync,实际 %s",
             (unsigned)v, decodeErrName(e));
    assertDecode(DecodeErr::BadSync, e, msg);
    snprintf(msg, sizeof(msg), "SYNC 换成 0x%02X 之后 CRC 判据变了(§2:SYNC 不进 CRC)", (unsigned)v);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(field, frameCrc(g, n), msg);
  }
}

// CRC 字段的**形态**：15 位值直接 2 字节大端，bit15 恒 0。
// ★ 两条反例（§2「校验」点的两处"别照搬"）都必须被拒：
//   ① 按 VAN 那套 fcsFieldFromCrc() 左移一位再存 → crc 错；
//   ② 字段的 bit15 被置 1 → crc 错（15 位值不可能有 bit15）。
static void test_link_frame_crc_field_is_15bit(void) {
  uint8_t payload[7];
  fillPayload(payload, 7, 0x77);
  uint8_t buf[kParseBufBytes];
  const uint16_t n = encodeFrame((uint8_t)MsgType::Data, payload, 7, kRoleSlave, buf, sizeof(buf));
  const uint16_t crc = frameCrc(buf, n);
  TEST_ASSERT_TRUE_MESSAGE(crc != 0u, "这个用例需要一个非 0 的 CRC(否则左移反例与正例无法区分)");
  TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)(buf[n - 2u] & 0x80u));      // bit15 恒 0
  TEST_ASSERT_EQUAL_HEX16(crc, (uint16_t)(((uint16_t)buf[n - 2u] << 8) | buf[n - 1u]));

  Frame f;
  // ① VAN 式左移（最低位恒 0）
  uint8_t g[kParseBufBytes];
  memcpy(g, buf, n);
  const uint16_t shifted = van::fcsFieldFromCrc(crc);
  g[n - 2u] = (uint8_t)(shifted >> 8);
  g[n - 1u] = (uint8_t)(shifted & 0xFFu);
  assertDecode(DecodeErr::CrcError, decodeFrame(g, n, &f),
               "字段按 VAN 那套左移过(§2 明确不许)却还收下了");

  // ② bit15 置 1
  memcpy(g, buf, n);
  g[n - 2u] = (uint8_t)(g[n - 2u] | 0x80u);
  assertDecode(DecodeErr::CrcError, decodeFrame(g, n, &f),
               "CRC 字段的 bit15 被置 1(15 位值不可能有 bit15)却还收下了");
}

// ------------------------------------------------------------
// 拒绝路径
// ------------------------------------------------------------
static void test_link_frame_reject_short_and_bad_sync(void) {
  uint8_t buf[kParseBufBytes];
  uint8_t payload[kDataLen];
  fillPayload(payload, kDataLen, 0x21);
  const uint16_t n = encodeFrame((uint8_t)MsgType::Data, payload, kDataLen, kRoleMaster,
                                 buf, sizeof(buf));
  Frame f;
  assertDecode(DecodeErr::ShortFrame, decodeFrame(buf, 0u, &f), "0 字节");
  assertDecode(DecodeErr::ShortFrame, decodeFrame(buf, kOverhead - 1u, &f), "6 字节(短一字节)");
  assertDecode(DecodeErr::ShortFrame, decodeFrame(nullptr, n, &f), "空指针");
  assertDecode(DecodeErr::ShortFrame, decodeFrame(buf, n, nullptr), "空输出");

  // 帧头的 SYNC 被换掉：0x56 = 回放行的 'V'（§2 特意让两者不撞），0x00/0xFF 是空闲/填充值
  const uint8_t notSync[] = {0x56u, 0x00u, 0xFFu, 0xA5u};
  for (uint8_t i = 0; i < sizeof(notSync); ++i) {
    uint8_t g[kParseBufBytes];
    memcpy(g, buf, n);
    g[kOffSync] = notSync[i];
    char msg[96];
    snprintf(msg, sizeof(msg), "SYNC = 0x%02X 的帧必须按坏帧头丢", (unsigned)notSync[i]);
    assertDecode(DecodeErr::BadSync, decodeFrame(g, n, &f), msg);
  }
}

static void test_link_frame_reject_len_mismatch(void) {
  uint8_t buf[kParseBufBytes];
  memset(buf, 0, sizeof(buf));
  uint8_t payload[kDataLen];
  fillPayload(payload, kDataLen, 0x42);
  const uint16_t n = encodeFrame((uint8_t)MsgType::Data, payload, kDataLen, kRoleMaster,
                                 buf, sizeof(buf));
  Frame f;
  assertDecode(DecodeErr::LenMismatch, decodeFrame(buf, (uint16_t)(n - 1u), &f),
               "少给 1 个字节(半截帧)");
  assertDecode(DecodeErr::LenMismatch, decodeFrame(buf, (uint16_t)(n + 1u), &f),
               "多给 1 个字节(LEN 与实际不符)");
  assertDecode(DecodeErr::LenMismatch, decodeFrame(buf, kParseBufBytes, &f),
               "给了整块缓冲(实际字节数远多于 LEN)");
  // 完整帧本身当然要过
  assertDecode(DecodeErr::Ok, decodeFrame(buf, n, &f), "完整帧");
}

// LEN 越界分两种，判据顺序是刻意分开的（见 decodeFrame 的注释）：
//   · LEN = 0..4 与 17..64：整帧收得下 ⇒ 先算 CRC。CRC 过了才报 len_range
//     （= 对端发了个 v1 认不了的长/短载荷，不是误码）；CRC 不过报 crc。
//   · LEN > 64：**不等载荷**，只给 7 个字节也立刻报 len_range。
static void test_link_frame_reject_len_out_of_range(void) {
  uint8_t payload[64];
  fillPayload(payload, 64, 0x55);

  // LEN 下界以下是 0..kLenMin-1；上界以上取 17/32/64（仍在"敢收完"的范围内）
  const uint8_t outLen[] = {0u, 1u, 2u, 3u, 17u, 32u, 64u};
  for (uint8_t i = 0; i < sizeof(outLen); ++i) {
    uint8_t buf[kParseBufBytes];
    const uint16_t n = buildFrameByHand((uint8_t)MsgType::Tick, payload, outLen[i],
                                        kRoleMaster, kVer, buf);
    Frame f;
    char msg[96];
    snprintf(msg, sizeof(msg), "LEN = %u(v1 合法范围是 %u..%u)必须丢帧计数",
             (unsigned)outLen[i], (unsigned)kLenMin, (unsigned)kLenMax);
    assertDecode(DecodeErr::LenOutOfRange, decodeFrame(buf, n, &f), msg);
    TEST_ASSERT_FALSE_MESSAGE(lenInRange(outLen[i]), "lenInRange 把越界值判成合法了");
  }

  // LEN > 64：不等载荷 —— 缓冲里只有 7 个字节也必须立刻判 len_range
  uint8_t buf[kParseBufBytes];
  for (uint16_t len = (uint16_t)kLenNoWaitAbove + 1u; len <= 255u; ++len) {
    memset(buf, 0, sizeof(buf));
    buf[kOffSync] = kSync;
    buf[kOffVer]  = kVer;
    buf[kOffType] = (uint8_t)MsgType::Data;
    buf[kOffLen]  = (uint8_t)len;
    buf[kOffRole] = kRoleMaster;
    Frame f;
    char msg[96];
    snprintf(msg, sizeof(msg), "LEN = %u 必须按坏帧丢、不等载荷", (unsigned)len);
    assertDecode(DecodeErr::LenOutOfRange, decodeFrame(buf, kOverhead, &f), msg);
  }
}

static void test_link_frame_reject_unknown_type(void) {
  uint8_t payload[kHelloLen];
  fillPayload(payload, kHelloLen, 0x63);
  // 0x00/0x05/0x0F/0x11/0x22/0x31/0x41 是"表里没有的号"；
  // 0x50 是从板文本日志转发（§6 的 v2 候选）、0x06 是保留的事件号（不是 TYPE）
  // ★ 2026-09-27：这个数组里原来有 **0x21** —— 那一号已经被 `MsgType::VanRaw`
  //   （原始帧转发）占用 ⇒ 它不再是"表里没有的号"，用例当场就会失败。
  //   换成仍然空着的 **0x22**：**号码被别人占了就要改这一行**，而这条注释就是
  //   留给人找的地方（下一次再占号时它会同样炸一次，这是有意的）。
  const uint8_t unknown[] = {0x00u, 0x05u, 0x06u, 0x0Fu, 0x11u, 0x22u, 0x31u, 0x41u, 0x50u, 0xFFu};
  for (uint8_t i = 0; i < sizeof(unknown); ++i) {
    uint8_t buf[kParseBufBytes];
    const uint16_t n = encodeFrame(unknown[i], payload, kHelloLen, kRoleMaster, buf, sizeof(buf));
    Frame f;
    char msg[96];
    snprintf(msg, sizeof(msg), "TYPE = 0x%02X 不在 §3 的表里,必须丢帧计数", (unsigned)unknown[i]);
    assertDecode(DecodeErr::UnknownType, decodeFrame(buf, n, &f), msg);
    TEST_ASSERT_FALSE(typeKnown(unknown[i]));
    TEST_ASSERT_EQUAL_UINT8(0u, payloadLenForType(unknown[i]));
  }
  // 表里的**六个** TYPE 一个都不能被误杀（2026-09-27 起含 VANRAW 0x21）
  const uint8_t known[] = {(uint8_t)MsgType::Hello, (uint8_t)MsgType::Tick,
                           (uint8_t)MsgType::Data, (uint8_t)MsgType::VanRaw,
                           (uint8_t)MsgType::Status, (uint8_t)MsgType::Event};
  for (uint8_t i = 0; i < sizeof(known); ++i) {
    uint8_t buf[kParseBufBytes];
    const uint16_t n = encodeFrame(known[i], payload, kHelloLen, kRoleMaster, buf, sizeof(buf));
    Frame f;
    char msg[96];
    snprintf(msg, sizeof(msg), "TYPE = 0x%02X 是 §3 的已知 TYPE,不该被丢", (unsigned)known[i]);
    assertDecode(DecodeErr::Ok, decodeFrame(buf, n, &f), msg);
    TEST_ASSERT_EQUAL_HEX8(known[i], f.type);
    TEST_ASSERT_EQUAL_STRING("unknown", msgTypeName(0x7Fu));
    TEST_ASSERT_TRUE(typeKnown(known[i]));
  }
}

// §2「版本不匹配时的行为」：主版本不同 = **上报 + 告警，不断链不降级**
//   · 已知 TYPE 照 v1 解析（ver_mismatch = 1）；
//   · 次版本不同连告警都没有（次版本只用来"加东西"）；
//   · 版本不匹配**不豁免**未知 TYPE 的丢弃。
static void test_link_frame_ver_mismatch_is_not_a_reject(void) {
  uint8_t payload[kHelloLen];
  fillPayload(payload, kHelloLen, 0x19);
  Frame f;

  // ① 主版本 2 + 已知 TYPE(TICK)：收下，但 ver_mismatch = 1
  uint8_t buf[kParseBufBytes];
  uint16_t n = encodeFrame((uint8_t)MsgType::Tick, payload, kHelloLen, kRoleMaster,
                           buf, sizeof(buf), 0x20u);
  assertDecode(DecodeErr::Ok, decodeFrame(buf, n, &f), "主版本不同的已知 TYPE 帧必须照解");
  TEST_ASSERT_TRUE(f.ver_mismatch);
  TEST_ASSERT_EQUAL_HEX8(0x20u, f.ver);
  TEST_ASSERT_EQUAL_HEX8(0x00u, (uint8_t)(f.ver & 0x0Fu));   // 次版本 0

  // ② 次版本不同(0x11)：**不告警**
  n = encodeFrame((uint8_t)MsgType::Tick, payload, kHelloLen, kRoleMaster, buf, sizeof(buf), 0x11u);
  assertDecode(DecodeErr::Ok, decodeFrame(buf, n, &f), "次版本不同的已知 TYPE 帧必须照解");
  TEST_ASSERT_FALSE_MESSAGE(f.ver_mismatch, "§2:次版本不同**不告警**");
  TEST_ASSERT_EQUAL_HEX8(0x11u, f.ver);

  // ③ 主版本不同 + 未知 TYPE：还是丢（版本不匹配不是"什么都收"）
  n = encodeFrame(0x50u, payload, kHelloLen, kRoleMaster, buf, sizeof(buf), 0x20u);
  assertDecode(DecodeErr::UnknownType, decodeFrame(buf, n, &f),
               "主版本不同不该豁免未知 TYPE 的丢弃");

  // ④ 主版本 0x10(=v1)、次版本 0xF：不告警
  n = encodeFrame((uint8_t)MsgType::Hello, payload, kHelloLen, kRoleSlave, buf, sizeof(buf), 0x1Fu);
  assertDecode(DecodeErr::Ok, decodeFrame(buf, n, &f), "0x1F 的主版本仍是 1");
  TEST_ASSERT_FALSE(f.ver_mismatch);
}

static void test_link_frame_encode_rejects_bad_args(void) {
  uint8_t payload[kLenMax];
  fillPayload(payload, kLenMax, 0x2A);
  uint8_t buf[kParseBufBytes];
  TEST_ASSERT_EQUAL_UINT16(0u, encodeFrame((uint8_t)MsgType::Tick, payload, 0u, kRoleMaster,
                                           buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_UINT16(0u, encodeFrame((uint8_t)MsgType::Tick, payload, kLenMin - 1u,
                                           kRoleMaster, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_UINT16(0u, encodeFrame((uint8_t)MsgType::Tick, payload, kLenMax + 1u,
                                           kRoleMaster, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_UINT16(0u, encodeFrame((uint8_t)MsgType::Tick, nullptr, kTickLen,
                                           kRoleMaster, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_UINT16(0u, encodeFrame((uint8_t)MsgType::Tick, payload, kTickLen,
                                           kRoleMaster, nullptr, 64u));
  // 容量差一格：要 7+5 = 12
  TEST_ASSERT_EQUAL_UINT16(0u, encodeFrame((uint8_t)MsgType::Tick, payload, kTickLen,
                                           kRoleMaster, buf, 11u));
  TEST_ASSERT_EQUAL_UINT16(12u, encodeFrame((uint8_t)MsgType::Tick, payload, kTickLen,
                                            kRoleMaster, buf, 12u));
  TEST_ASSERT_EQUAL_UINT16(23u, encodeFrame((uint8_t)MsgType::Status, payload, kLenMax,
                                            kRoleMaster, buf, 23u));
  TEST_ASSERT_EQUAL_UINT8(0u, payloadLenForType(0x99u));
  TEST_ASSERT_EQUAL_UINT8(kDataLen, payloadLenForType((uint8_t)MsgType::Data));
}

// §2 的第 4 字节 ROLE + §5 的角色判据（宏、冲突、两条自检）
static void test_link_frame_role_and_selfchecks(void) {
  uint8_t payload[kEventLen];
  fillPayload(payload, kEventLen, 0x0B);
  const uint8_t roles[] = {kRoleSlave, kRoleMaster};
  for (uint8_t i = 0; i < 2; ++i) {
    uint8_t buf[kParseBufBytes];
    const uint16_t n = encodeFrame((uint8_t)MsgType::Event, payload, kEventLen, roles[i],
                                   buf, sizeof(buf));
    Frame f;
    assertDecode(DecodeErr::Ok, decodeFrame(buf, n, &f), "两个 ROLE 值都必须能往返");
    TEST_ASSERT_EQUAL_UINT8(roles[i], f.role);
  }

  // ★ 默认值：没定义 LINK_ROLE 时 = 0 = 从板(被动侧)。理由见 link_role.h。
  TEST_ASSERT_EQUAL_UINT8(kRoleSlave, kLocalRole);
  TEST_ASSERT_EQUAL_STRING("slave(B/left)", roleName(kRoleSlave));
  TEST_ASSERT_EQUAL_STRING("master(A/right)", roleName(kRoleMaster));

  // §5 ①：对端 ROLE == 本机 ROLE ⇒ 角色冲突(丢帧 + 报警)
  TEST_ASSERT_TRUE(roleConflict(kRoleMaster, kRoleMaster));
  TEST_ASSERT_TRUE(roleConflict(kRoleSlave, kRoleSlave));
  TEST_ASSERT_FALSE(roleConflict(kRoleMaster, kRoleSlave));

  // §5 ②：主板开着 VAN 物理层却 edges 恒 0 + 收到了 DATA ⇒ "本机像从板"
  TEST_ASSERT_TRUE(masterSelfCheckLooksLikeSlave(kRoleMaster, true, 0u, true));
  TEST_ASSERT_FALSE(masterSelfCheckLooksLikeSlave(kRoleSlave, true, 0u, true));
  TEST_ASSERT_FALSE(masterSelfCheckLooksLikeSlave(kRoleMaster, true, 123u, true));
  TEST_ASSERT_FALSE(masterSelfCheckLooksLikeSlave(kRoleMaster, false, 0u, true));
  TEST_ASSERT_FALSE(masterSelfCheckLooksLikeSlave(kRoleMaster, true, 0u, false));
  // §5 ③：从板却在本地看到 VAN 边沿 ⇒ "这块板才该是主板"
  TEST_ASSERT_TRUE(slaveSelfCheckLooksLikeMaster(kRoleSlave, 1u));
  TEST_ASSERT_FALSE(slaveSelfCheckLooksLikeMaster(kRoleSlave, 0u));
  TEST_ASSERT_FALSE(slaveSelfCheckLooksLikeMaster(kRoleMaster, 1u));
}

void register_link_frame_tests(void) {
  RUN_TEST(test_link_frame_layout_and_roundtrip);
  RUN_TEST(test_link_frame_len_bounds_roundtrip);
  RUN_TEST(test_link_frame_crc_covers_ver_to_payload);
  RUN_TEST(test_link_frame_sync_outside_crc);
  RUN_TEST(test_link_frame_crc_field_is_15bit);
  RUN_TEST(test_link_frame_reject_short_and_bad_sync);
  RUN_TEST(test_link_frame_reject_len_mismatch);
  RUN_TEST(test_link_frame_reject_len_out_of_range);
  RUN_TEST(test_link_frame_reject_unknown_type);
  RUN_TEST(test_link_frame_ver_mismatch_is_not_a_reject);
  RUN_TEST(test_link_frame_encode_rejects_bad_args);
  RUN_TEST(test_link_frame_role_and_selfchecks);
}

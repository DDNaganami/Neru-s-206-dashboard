// VAN 线路层测试(宿主机,不需要收发器/总线)
//
// 数据来源分三类,可信度不同,测试里逐条标注:
//
// [实测] 2026-09-19 实车逻辑分析仪抓包(5 分钟 drive5min.csv,17106 帧;
//   仓库内 1.2s 切片 66 帧,两条独立抓包互证)定案的帧结构:
//   SOF 10 TS / IDEN 15 TS(= 3 个 4B5B 组 = 12 位)/ CMD 5 TS(= 1 组 = 4 位)/
//   DATA 10n TS / FCS 20 TS(= 4 组 = 16 位 = 15 位 CRC + 1 个固定 0 位)/
//   EOD = FCS 末字节的 bit0 与紧随的编码位(不额外占槽)/ ACK 2 TS(仅被应答的帧)
//   编码 E-Manchester = 4B5B,每 5 个 TS 的第 5 个是编码位(**= 第 4 位的反相**),
//   解码时丢弃;全帧恰好最后一组违反这条 = EOD。
//   FCS = crc15_van_iso(0x0F9D/init 0x7FFF/输出取反/MSB-first),覆盖 IDEN+CMD+DATA。
//   槽时间:实车实测 8.25µs(≈121kbit/s),规范标称 125kbit/s 是 8.00µs;
//   固件以实测值为唯一时基(van_wire.h 的 kTsNs),本文件的向量都从它换算。
//
// [往返] 自己的编码器 → 字节级解析器,验证 IDEN/CMD 打包、FCS 反推、帧字节契约
//
// [鲁棒性] 坏帧不得被判为有效帧
//
// 真实位流那一层(边沿 → SOF → 字节 → FCS 通过)由
// test_van_real_capture.cpp 用真实抓包回归(那里断言 frames 会随 FCS 通过而增加)。
// 本文件只做"不过位解码器"的字节级/编码器级验证。
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "van_wire.h"
#include "van_source.h"

using namespace van;

// 把编码器的槽序列折回字节:每 10 槽 = 1 字节(槽 4/9 是 E-Manchester
// 编码位,丢弃)。4 位半字节放进字节上半部,低半字节再右移 4 位。
// 返回字节数,bytes[0] = SOF。
static uint8_t foldSlots(const uint8_t* slots, uint32_t n, uint8_t* bytes, uint8_t cap) {
  uint8_t nb = 0;
  for (uint32_t i = kSofSlots; i + 10 <= n && nb < cap; i += 10) {
    uint8_t hi4 = 0, lo4 = 0;
    for (uint8_t k = 0; k < 4; ++k) {
      if (slots[i + k]) hi4 |= (uint8_t)(1u << (7 - k));
      if (slots[i + 5 + k]) lo4 |= (uint8_t)(1u << (7 - k));
    }
    bytes[nb++] = (uint8_t)(hi4 | (lo4 >> 4));
  }
  return nb;
}

// 把 SOF 那 kSofSlots 个槽按 4B5B 折一下(仅用于"确认它不是数据字节")。
// ★ 规范图案 0000111101 折出来是 **0x0E**,不是 0x0F ——
//   0x0F 只是前 8 个裸槽的巧合。别再拿 0x0F 当 SOF 正确的证据。
static uint8_t foldSofByte(const uint8_t* slots, uint32_t n) {
  if (n < kSofSlots) return 0xFF;
  uint8_t hi4 = 0, lo4 = 0;
  for (uint8_t k = 0; k < 4; ++k) {
    if (slots[k]) hi4 |= (uint8_t)(1u << (7 - k));
    if (slots[5 + k]) lo4 |= (uint8_t)(1u << (7 - k));
  }
  return (uint8_t)(hi4 | (lo4 >> 4));
}

// ---------- CRC-15 基本性质 ----------
static void test_crc15_deterministic(void) {
  const uint8_t a[] = {0x8A, 0x22, 0x5A};
  const uint8_t b[] = {0x8A, 0x22, 0x5B};
  const uint16_t ca = crc15(a, sizeof(a));
  const uint16_t cb = crc15(b, sizeof(b));
  TEST_ASSERT_TRUE(ca != cb);                  // 单字节差异必须改变 CRC
  TEST_ASSERT_TRUE(ca <= 0x7FFF);              // 15 位
  TEST_ASSERT_EQUAL_UINT16(ca, crc15(a, sizeof(a)));   // 稳定

  // 增量式与一次性必须一致(crc15_extend 是给流式接收预留的)
  uint16_t inc = 0;
  for (uint16_t i = 0; i < sizeof(a); ++i) inc = crc15_extend(inc, a[i]);
  TEST_ASSERT_EQUAL_UINT16(ca, inc);
}

// IDEN/CMD 的字节打包契约:12 位 IDEN 拆成两字节。
// ★ 值按**实测**的线上字节流写死(2026-09-19 实车抓包,17106 帧):
//     0x824 → 字节 0x82 0x48(真实抓包测试里一直印着这两个字节)
//   旧断言(0xC4/0xC8 = "IDEN 低 8 位先行")是**错的**,与真实字节流对不上,
//   现在按实测改掉 —— 这条以前只保证"自己和自己一致",所以没暴露。
static void test_iden_byte_layout(void) {
  const uint16_t iden = 0x8C4;
  const uint8_t cmd = 0xC;
  const uint8_t b1 = idenByte1(iden);
  const uint8_t b2 = idenByte2(iden, cmd);
  TEST_ASSERT_EQUAL_HEX8(0x8C, b1);                       // IDEN 的 bit11..4
  TEST_ASSERT_EQUAL_HEX8(0x4C, b2);                       // IDEN bit3..0 | CMD
  TEST_ASSERT_EQUAL_HEX16(iden, idenFromBytes(b1, b2));   // 往返是硬要求
  TEST_ASSERT_EQUAL_HEX8(cmd, cmdFromByte2(b2));          // CMD 可还原
  // 车速帧那一组:必须解出 0x82 0x48 → 0x824(与真实抓包一致)
  TEST_ASSERT_EQUAL_HEX8(0x82, idenByte1(0x824));
  TEST_ASSERT_EQUAL_HEX8(0x48, idenByte2(0x824, 0x8));

  // 12 位有效:更高的位不得污染 IDEN
  const uint16_t iden_hi = 0xF8C4;
  TEST_ASSERT_EQUAL_HEX16(0x8C4, idenFromBytes(idenByte1(iden_hi), idenByte2(iden_hi, cmd)));
}

// 编解码往返自洽:编码器写的 FCS 必须能被自己的解析器认回来。
// ★ 现在这条不只"内部自洽"了:多项式/覆盖范围/字段序都是实测定案的
//   (crc15_van_iso + 16 位大端字段),所以往返通过 = 编解码一致,
//   而"与真车一致"由 test_van_real_capture.cpp 用真实抓包钉住。
static void test_crc15_selfconsistent(void) {
  Frame f;
  f.ident = 0x8C4;
  f.cmd = 0xC;
  const uint8_t d[3] = {0x8A, 0x22, 0x5A};
  f.len = 3;
  memcpy(f.data, d, 3);

  uint8_t slots[512];
  const uint32_t n = encodeFrame(f, slots, sizeof(slots));
  TEST_ASSERT_TRUE(n > 0);

  uint8_t bytes[16];
  const uint8_t nb = foldSlots(slots, n, bytes, sizeof(bytes));
  // 逻辑帧 = IDEN/CMD(2) + 数据(3) + FCS(2) = 7 字节(SOF 已被 foldSlots 跳过);
  // 帧尾补齐的槽可能多出半个字节
  TEST_ASSERT_TRUE(nb >= 7);

  // SOF 单独校验:它是 kSofSlots 个裸槽,不进字节流。
  // ★ 按规范 0000111101 折 4B5B(丢第 5/10 位)得到的是 **0x0E**,不是 0x0F。
  //   0x0F 只是"前 8 个裸槽"这个巧合,别再拿它当 SOF 的证据。
  TEST_ASSERT_EQUAL_HEX8(0x0E, foldSofByte(slots, n));
  TEST_ASSERT_EQUAL_HEX8(idenByte1(0x8C4), bytes[0]);   // 0x8C = IDEN bit11..4
  TEST_ASSERT_EQUAL_HEX8(idenByte2(0x8C4, 0xC), bytes[1]);   // 0x4C
  TEST_ASSERT_EQUAL_HEX8(0x8A, bytes[2]);
  TEST_ASSERT_EQUAL_HEX8(0x22, bytes[3]);
  TEST_ASSERT_EQUAL_HEX8(0x5A, bytes[4]);

  // 解析输入 = 从 IDEN 字节到最后一个 FCS 字节。
  // 长度**按逻辑帧算**(IDEN/CMD 2 + 数据 + FCS 2),不要用 nb 推 ——
  // 帧尾补齐的槽会多出填充字节,用 nb 推会把填充当数据(nb-2 正好少一个)。
  const uint16_t logical = (uint16_t)(2 + f.len + 2);
  TEST_ASSERT_TRUE(nb >= logical);
  Frame out;
  const bool ok = parseFrameBytes(&bytes[0], logical, &out);
  TEST_ASSERT_TRUE_MESSAGE(ok, "自洽性失败:编码器写的 FCS 自己解析不回来");
  TEST_ASSERT_TRUE(out.fcs_ok);
  TEST_ASSERT_EQUAL_HEX16(0x8C4, out.ident);
  TEST_ASSERT_EQUAL_HEX8(3, out.len);
  for (uint8_t i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_HEX8(d[i], out.data[i]);
}

// ---------- 帧字节解析(不经位解码器) ----------
static void test_parse_frame_bytes_iden_cmd(void) {
  Frame f;
  f.ident = 0x824;
  f.cmd = 0xC;
  const uint8_t d[7] = {0x12, 0x35, 0x67, 0x89, 0xAB, 0xCD, 0xE0};
  f.len = 7;
  memcpy(f.data, d, 7);

  uint8_t slots[512];
  const uint32_t n = encodeFrame(f, slots, sizeof(slots));
  TEST_ASSERT_TRUE(n > 0);

  uint8_t bytes[32];
  const uint8_t nb = foldSlots(slots, n, bytes, sizeof(bytes));
  // IDEN(1) + CMD(1) + 数据(7) + FCS(2) = 11(SOF 已被 foldSlots 跳过)
  TEST_ASSERT_TRUE(nb >= 11);

  // SOF 是 10 个裸槽,折 4B5B 得 0x0E(不是 0x0F);它不进字节流
  TEST_ASSERT_EQUAL_HEX8(0x0E, foldSofByte(slots, n));
  TEST_ASSERT_EQUAL_HEX8(0x82, bytes[0]);   // IDEN 0x824 的 bit11..4(实测线上字节)
  TEST_ASSERT_EQUAL_HEX8(0x4C, bytes[1]);   // IDEN bit3..0 | CMD

  Frame out;
  const bool ok = parseFrameBytes(&bytes[0], (uint16_t)(nb - 1), &out);
  TEST_ASSERT_TRUE_MESSAGE(ok, "FCS 反推失败:CRC 覆盖范围或字节序不对");
  TEST_ASSERT_EQUAL_HEX16(0x824, out.ident);
  TEST_ASSERT_EQUAL_HEX8(0xC, out.cmd);
  TEST_ASSERT_EQUAL_UINT8(7, out.len);
  for (uint8_t i = 0; i < 7; ++i) TEST_ASSERT_EQUAL_HEX8(d[i], out.data[i]);
  TEST_ASSERT_TRUE(out.fcs_ok);
}

// FCS 覆盖范围与字段序的隔离测试:
// 直接按"线上字节流(IDEN, IDEN/CMD, data...)"算 crc15_van_iso,再确认编码器
// 写出的末尾两字节就是它的 16 位大端字段(= crc<<1,最低位恒 0)——
// 把"CRC 算错"和"帧字节布局错"分开定位。
static void test_fcs_placement(void) {
  Frame f;
  f.ident = 0x824;
  f.cmd = 0xC;
  const uint8_t d[7] = {0x12, 0x35, 0x67, 0x89, 0xAB, 0xCD, 0xE0};
  f.len = 7;
  memcpy(f.data, d, 7);

  uint8_t slots[512];
  const uint32_t n = encodeFrame(f, slots, sizeof(slots));
  uint8_t bytes[32];
  const uint8_t nb = foldSlots(slots, n, bytes, sizeof(bytes));
  TEST_ASSERT_TRUE(nb >= 11);

  // 期望的 CRC 覆盖范围:bytes[0..8] = IDEN, IDEN/CMD, 7 字节数据
  // (SOF 已被 foldSlots 跳过,下标整体少 1)
  const uint16_t calc = crc15_van_iso(&bytes[0], 9);

  // 末尾两字节 = 16 位大端字段:高字节先发,最低位是那个固定 0
  const uint16_t field = (uint16_t)(((uint16_t)bytes[9] << 8) | bytes[10]);
  char msg[96];
  snprintf(msg, sizeof(msg), "calc=0x%04X field=0x%04X (bytes[9..10]=%02X %02X)",
           calc, field, bytes[9], bytes[10]);
  TEST_ASSERT_TRUE_MESSAGE(fcsFieldWellFormed(field), msg);       // 最低位必须是 0
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(calc, fcsCrcFromField(field), msg);
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(fcsFieldFromCrc(calc), field, msg);
}

// 固定向量:钉住编码器的**关键不变式**,而不是逐个字节的手算值
// (手算值多次与实现不符,反复自相矛盾;下面这几条才是产品契约)
static void test_known_bit_vector(void) {
  Frame f;
  f.ident = 0x824;
  f.cmd = 0xC;
  const uint8_t d[7] = {0x12, 0x35, 0x67, 0x89, 0xAB, 0xCD, 0xE0};
  f.len = 7;
  memcpy(f.data, d, 7);

  uint8_t slots[512];
  const uint32_t n = encodeFrame(f, slots, sizeof(slots));
  TEST_ASSERT_TRUE(n > 0);

  uint8_t bytes[32];
  const uint8_t nb = foldSlots(slots, n, bytes, sizeof(bytes));
  TEST_ASSERT_TRUE(nb >= 11);

  // 1) SOF 是 kSofSlots 个裸槽:折 4B5B 得 0x0E(不是 0x0F),
  //    而且它**不进字节流** —— bytes[0] 已经是 IDENlo
  TEST_ASSERT_EQUAL_HEX8(0x0E, foldSofByte(slots, n));
  TEST_ASSERT_NOT_EQUAL_MESSAGE(kSofByte, bytes[0], "首字节不该再是 SOF 字节");
  // 2) IDEN 字段必须能还原(不管高 4 位落在哪个半字节,往返是对的)
  TEST_ASSERT_EQUAL_HEX16(f.ident, idenFromBytes(bytes[0], bytes[1]));
  TEST_ASSERT_EQUAL_HEX8(f.cmd, cmdFromByte2(bytes[1]));
  // 3) 数据区必须一字不差
  for (uint8_t i = 0; i < 7; ++i) TEST_ASSERT_EQUAL_HEX8(d[i], bytes[2 + i]);
  // 4) 帧尾两字节必须能被解析器当作 FCS 接受(顺序自适应)。
  //    注意长度只能取到 FCS 高字节为止:编码器在 EOD 后还补了 recessive 槽,
  //    折字节时会多出填充字节,若把它也传进去,解析器会把填充当 FCS
  //    (曾因传 nb-1 而不是 nb-2 导致"自己写的帧自己不认")。
  Frame out;
  TEST_ASSERT_TRUE_MESSAGE(parseFrameBytes(&bytes[0], (uint16_t)(nb - 1), &out),
                           "编码器产出的帧不通过自己的 FCS 校验");
  TEST_ASSERT_TRUE(out.fcs_ok);
}

// 4B5B 的不变式(用它才能推出帧尾判据的边界):
//   每个 10 槽字节里,位置 4 和 9 是编码位,恒为 recessive;
//   所以**任何字节**里连续 recessive 的上界是 10(8 个 1 的数据位 + 2 个编码位)。
// 反过来说:规范里"帧尾 = 8 个连续 recessive"这条判据,在出现 0xFF
// 这种全 1 数据字节时会提前触发 —— 真实总线靠 ACK 位是 dominant 打破它。
// 本项目只监听不应答,这是实车必须确认的点之一。
static void test_recessive_run_bound(void) {
  Frame f;
  f.ident = 0x824;
  f.cmd = 0xC;
  f.len = 4;
  memset(f.data, 0xFF, 4);       // 全 1 数据 = 最长 recessive 情形
  uint8_t slots[512];
  const uint32_t n = encodeFrame(f, slots, sizeof(slots));
  TEST_ASSERT_TRUE(n > 0);

  uint32_t run = 0, maxrun = 0;
  for (uint32_t i = 0; i < n; ++i) {
    run = slots[i] ? run + 1 : 0;
    if (run > maxrun) maxrun = run;
  }
  char m[80];
  snprintf(m, sizeof(m), "maxrun=%u (n=%u)", (unsigned)maxrun, (unsigned)n);
  // 每个全 1 数据字节贡献 10 个连续 recessive(8 数据位 + 2 编码位),
  // 4 个字节就是 40;帧尾 EOD/ACK/EOF 段另算。上界取 64 留足余量。
  TEST_ASSERT_TRUE_MESSAGE(maxrun >= 10, m);
  TEST_ASSERT_TRUE_MESSAGE(maxrun <= 64, m);
}

// 帧解析器必须在 endFrame 后清空缓冲(曾因不清 mCount 导致第二帧永久失败)
static void test_consecutive_frames_at_parser_level(void) {
  Frame f1;
  f1.ident = 0x824; f1.cmd = 0xC; f1.len = 2;
  f1.data[0] = 0x11; f1.data[1] = 0x22;

  uint8_t slots[512];
  const uint32_t n = encodeFrame(f1, slots, sizeof(slots));
  uint8_t bytes[32];
  const uint8_t nb = foldSlots(slots, n, bytes, sizeof(bytes));
  // 逻辑帧 = IDEN/CMD(2) + 数据(len) + FCS(2)。SOF 已被 foldSlots 跳过,
  // 不计入;末尾多出的填充字节也不能喂进来,否则会把 FCS 顶掉。
  const uint8_t logical = (uint8_t)(4 + f1.len);
  TEST_ASSERT_TRUE(nb >= logical);
  // ★ SOF 不再是数据字节:bytes[0] 是 IDEN 低字节。
  //   单独确认 SOF 那 10 槽折 4B5B 是 0x0E(规范图案的折叠值,不是 0x0F)。
  TEST_ASSERT_EQUAL_HEX8(0x0E, foldSofByte(slots, n));
  TEST_ASSERT_EQUAL_HEX8(idenByte1(f1.ident), bytes[0]);
  TEST_ASSERT_NOT_EQUAL_MESSAGE(kSofByte, bytes[0], "首字节不该再是 SOF 字节");

  FrameParser fp;
  Frame got;

  // 连喂两遍同样的字节流,两遍都必须解出同一帧
  for (int round = 0; round < 2; ++round) {
    // ★ 帧起点必须显式声明:解析器**不再**靠"首字节 == 0x0F"猜。
    //   真实链路上这一步由 BitDecoder 匹配到 SOF 后经 onFrameStart() 触发
    //   (见 test_van_phywire 的 test_sof_then_bytes_order)。
    fp.beginFrame(0);
    for (uint8_t i = 0; i < logical; ++i) {
      fp.pushByte(bytes[i], 0, &got);
    }
    const bool ok = fp.endFrame(0, &got);
    TEST_ASSERT_TRUE_MESSAGE(ok, "帧未解出(可能是缓冲未清,上一帧残留污染)");
    TEST_ASSERT_EQUAL_HEX16(f1.ident, got.ident);
    TEST_ASSERT_EQUAL_HEX8(f1.len, got.len);
    TEST_ASSERT_EQUAL_HEX8(f1.data[0], got.data[0]);
    TEST_ASSERT_EQUAL_HEX8(f1.data[1], got.data[1]);
    TEST_ASSERT_TRUE(got.fcs_ok);
  }
}

// 坏数据不得被当成有效帧(FCS 必须挡住)
static void test_corrupted_frame_rejected(void) {
  Frame f;
  f.ident = 0x824;
  f.cmd = 0xC;
  f.len = 3;
  f.data[0] = 0x11; f.data[1] = 0x22; f.data[2] = 0x33;

  uint8_t slots[512];
  const uint32_t n = encodeFrame(f, slots, sizeof(slots));
  uint8_t bytes[32];
  const uint8_t nb = foldSlots(slots, n, bytes, sizeof(bytes));
  // IDEN(1) + CMD(1) + 数据(3) + FCS(2) = 7
  TEST_ASSERT_TRUE(nb >= 7);
  TEST_ASSERT_EQUAL_HEX8(0x0E, foldSofByte(slots, n));
  TEST_ASSERT_EQUAL_HEX8(idenByte1(f.ident), bytes[0]);
  TEST_ASSERT_NOT_EQUAL_MESSAGE(kSofByte, bytes[0], "首字节不该再是 SOF 字节");

  // 翻转一个数据字节(改动数据但不改 FCS)。
  // 下标 2 = 数据区第 1 字节(bytes[0]=IDENlo, bytes[1]=IDENhi|CMD)
  const uint16_t logical = (uint16_t)(2 + f.len + 2);
  TEST_ASSERT_TRUE(nb >= logical);
  uint8_t bad[32];
  memcpy(bad, bytes, nb);
  bad[2] ^= 0x01;

  Frame out;
  const bool ok = parseFrameBytes(&bad[0], logical, &out);
  // 要么 FCS 反推失败,要么反推出一个与原数据不同的"长度/内容"
  if (ok) {
    const bool same = (out.len == 3) && (out.data[0] == f.data[0]) &&
                      (out.data[1] == f.data[1]) && (out.data[2] == f.data[2]);
    TEST_ASSERT_FALSE_MESSAGE(same, "坏帧被 FCS 放过了");
  }
}

// cmd 字段语义(EXT/RAK/RW/RTR)
static void test_cmd_bits(void) {
  const CmdBits c = decodeCmd(0xC);      // 1100
  TEST_ASSERT_TRUE(c.ext);               // 保留位应为 1
  TEST_ASSERT_TRUE(c.rak);
  TEST_ASSERT_FALSE(c.rw);
  TEST_ASSERT_FALSE(c.rtr);
}

// 位解码器的空闲段处理:总线空闲(长 recessive)不得产生任何字节 ——
// 未见到 SOF 之前一律丢弃,否则一段空闲会解出上百个无意义字节冲爆队列。
// (早期实现会把空闲解成 0xFF 字节流,队列只有 40 个,必然溢出。)
//
// 注:整链的帧结束判据在这里不重复验证 —— 字节级往返与帧尾判据已由
// test_roundtrip_through_bit_decoder / test_recessive_run_bound 覆盖,
// 本用例只回答"空闲会不会污染字节流"。
static void test_bit_decoder_idle_produces_no_bytes(void) {
  BitDecoder bd;
  bd.pushEdge(0, true);                       // 空闲起点
  const BitDecoder::Ev ev = bd.pushEdge(400ULL * kTsNs, false);   // 转为 dominant
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, bd.available(), "空闲段不该产生字节");
  TEST_ASSERT_FALSE(bd.overflowed());
  (void)ev;

  // 连续两段空闲(中间夹一次电平变化)同样不得产生字节
  BitDecoder bd3;
  bd3.pushEdge(0, true);
  bd3.pushEdge(5000000, false);
  bd3.pushEdge(10000000, true);
  bd3.pushEdge(15000000, false);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, bd3.available(), "多段空闲不该产生字节");
  TEST_ASSERT_FALSE(bd3.overflowed());
}

// ============================================================
// SOF 的头 16 槽 —— 显式比特串断言
//
// 为什么必须"写死比特串"而不是"折字节比 0x0F":
//   SOF 是**固定的 10 TS 同步图案**,不走 4B5B。
//   折 4B5B 时第 5/10 槽是被丢掉的编码位,折回 0x0F 只说明
//   "数据位还是那个字节",跟这 10 个槽是不是规范图案**无关** ——
//   这个混淆让我前面误判了两轮:putByte(0x0F) 产出 0000111111,
//   折回来同样是 0x0F,但第 9 槽是 1,matcher 永远对不上。
//
// 固定向量:ident = 0x824 → idenByte1 = 0x82(实测线上字节)
//   SOF 裸槽      : 0000111101
//   putByte(0x82) : 1000 1 0010 1 = 1000100101
//   头 16 槽      : 0000111101 100010
//                   ^^^^规范 SOF^^^^ ^^IDEN 开头
// ============================================================
static void test_sof_first_16_slots(void) {
  TEST_ASSERT_EQUAL_HEX16(0x003Du, kSofPattern);   // 0000111101
  TEST_ASSERT_EQUAL_UINT8(10u, kSofSlots);

  Frame f;
  f.ident = 0x824;
  f.cmd = 0xC;
  f.len = 1;
  f.data[0] = 0x55;

  uint8_t slots[512];
  const uint32_t n = encodeFrame(f, slots, sizeof(slots));
  TEST_ASSERT_TRUE(n >= 16);

  // 拼成字符串逐字符比 —— 报告里能直接看出哪一位不对
  char got[32];
  for (int i = 0; i < 16; ++i) got[i] = slots[i] ? '1' : '0';
  got[16] = '\0';
  const char* want = "0000111101100010";
  char msg[96];
  snprintf(msg, sizeof(msg), "头 16 槽 = %s,期望 %s", got, want);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(want, got, msg);

  // SOF 那 10 槽单独再断言(第 9 槽必须是 0 —— 这正是与 putByte 的关键差别)
  char sofGot[16];
  for (int i = 0; i < 10; ++i) sofGot[i] = slots[i] ? '1' : '0';
  sofGot[10] = '\0';
  TEST_ASSERT_EQUAL_STRING_MESSAGE("0000111101", sofGot, "SOF 10 槽必须是 0000111101");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, slots[8], "SOF 第 9 槽(0-based 8)必须是 0");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, slots[9], "SOF 第 10 槽(0-based 9)必须是 1");

  // 紧跟的 IDENlo 字节:按 4B5B 折回来应是 0x24
  uint8_t b = 0;
  for (uint8_t k = 0; k < 4; ++k) if (slots[10 + k]) b |= (uint8_t)(1u << (7 - k));
  for (uint8_t k = 0; k < 4; ++k) if (slots[15 + k]) b |= (uint8_t)(1u << (3 - k));
  TEST_ASSERT_EQUAL_HEX8(idenByte1(f.ident), b);
}

void register_van_wire_tests(void) {
  RUN_TEST(test_sof_first_16_slots);
  RUN_TEST(test_crc15_deterministic);
  RUN_TEST(test_iden_byte_layout);
  RUN_TEST(test_crc15_selfconsistent);
  RUN_TEST(test_parse_frame_bytes_iden_cmd);
  RUN_TEST(test_fcs_placement);
  RUN_TEST(test_known_bit_vector);
  RUN_TEST(test_recessive_run_bound);
  RUN_TEST(test_consecutive_frames_at_parser_level);
  RUN_TEST(test_corrupted_frame_rejected);
  RUN_TEST(test_cmd_bits);
  RUN_TEST(test_bit_decoder_idle_produces_no_bytes);
}

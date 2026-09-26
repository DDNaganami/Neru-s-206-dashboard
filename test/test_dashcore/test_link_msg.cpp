// 双板链路协议 v1 —— **消息载荷**用例（契约：ARCHITECTURE.md §3「消息表 v1」）
//
// 逐字节对契约表：字段宽度、字节序、量纲（rpm×8 / 1 计数 = 2.56 km/h / ℃+40）、
// DATA 的"每字段 2 位来源"、STATUS 的 16 字节、EVENT 的事件号段，
// 以及"帧长 = 7 + LEN"跟 §2 给的四个帧长对账。
//
// ★ 契约没写、由 lib/link/link_msg.h 定下来的三处口径（多字节大端 /
//   flags 的 2 位分配 / STATUS.flags 的位号）在本文件里被**显式钉住**：
//   它们不是"实现细节"，两板固件不一致就会静默错位。
#include <unity.h>
#include <limits>
#include <string.h>

#include "data_service.h"   // FieldSource —— 只为对账 2 位编码（数值必须一致）
#include "expression.h"     // Face —— STATUS.left_face / EVENT.face 的槽位下标
#include "link_frame.h"
#include "link_msg.h"

using namespace dashlink;

// 2 位来源编码与 data_service.h 的 FieldSource **数值一一对应**（编译期就炸，
// 免得以后有人给 FieldSource 插一档、把链路上的编码挤歪）。
static_assert((uint8_t)FieldSource::None == (uint8_t)Src::None, "FieldSource::None 必须 = 0");
static_assert((uint8_t)FieldSource::Sim  == (uint8_t)Src::Sim,  "FieldSource::Sim 必须 = 1");
static_assert((uint8_t)FieldSource::Obd  == (uint8_t)Src::Obd,  "FieldSource::Obd 必须 = 2");
static_assert((uint8_t)FieldSource::Van  == (uint8_t)Src::Van,  "FieldSource::Van 必须 = 3");

namespace {

void fillHello(HelloMsg* m) {
  m->fw_ver = 0x0102u;
  m->build_tag = 0xABCDu;
  m->boot_reason = 0x7Fu;
}
void fillTick(TickMsg* m) {
  m->tick_ms = 0x01020304u;
  m->seq = 0xFEu;
}
void fillData(DataMsg* m) {
  m->rpm_raw = 0x1CA2u;   // 7330 = 916.25 rpm
  m->speed_raw = 0x27u;   // 39 计数 = 99.84 km/h
  m->coolant_raw = 0x10u; // 16 → -24℃
  m->intake_raw = 0x00u;  // 0 → -40℃
  m->flags = 0x1Bu;       // rpm=Sim(01) speed=Obd(10) coolant=Van(11) intake=None(00)
}
void fillStatus(StatusMsg* m) {
  m->fw_ver = 0x0102u;
  m->uptime_ms = 0x00010203u;
  m->frames_ok = 0x1122u;
  m->frames_dropped = 0x3344u;
  m->crc_err = 0x5566u;
  m->last_gap_ms = 0x7788u;
  m->left_face = (uint8_t)Face::Sport;
  m->flags = (uint8_t)(kStFlagVerMismatch | kStFlagTempNoSource);
}
void fillEvent(EventMsg* m) {
  m->evt_id = (uint8_t)EvtId::FaceChange;
  m->value = 0x1234u;
  m->face = (uint8_t)Face::City;
}

}  // namespace

// ------------------------------------------------------------
// 逐字节布局 + 字节序（全部大端；只有 DATA.rpm_raw 在契约里显式写了 "BE"）
// ------------------------------------------------------------
static void test_link_msg_byte_layout_and_endianness(void) {
  uint8_t p[kStatusLen + 4];
  memset(p, 0xEE, sizeof(p));

  HelloMsg h;
  fillHello(&h);
  TEST_ASSERT_TRUE(packHello(h, p));
  TEST_ASSERT_EQUAL_HEX8(0x01u, p[0]);   // fw_ver = 0x0102,高字节先
  TEST_ASSERT_EQUAL_HEX8(0x02u, p[1]);
  TEST_ASSERT_EQUAL_HEX8(0xABu, p[2]);   // build_tag = 0xABCD
  TEST_ASSERT_EQUAL_HEX8(0xCDu, p[3]);
  TEST_ASSERT_EQUAL_HEX8(0x7Fu, p[4]);   // 第 5 个字节是载荷末尾

  TickMsg t;
  fillTick(&t);
  TEST_ASSERT_TRUE(packTick(t, p));
  TEST_ASSERT_EQUAL_HEX8(0x01u, p[0]);
  TEST_ASSERT_EQUAL_HEX8(0x02u, p[1]);
  TEST_ASSERT_EQUAL_HEX8(0x03u, p[2]);
  TEST_ASSERT_EQUAL_HEX8(0x04u, p[3]);
  TEST_ASSERT_EQUAL_HEX8(0xFEu, p[4]);

  DataMsg d;
  fillData(&d);
  TEST_ASSERT_TRUE(packData(d, p));
  TEST_ASSERT_EQUAL_HEX8(0x1Cu, p[0]);   // rpm_raw 高字节先
  TEST_ASSERT_EQUAL_HEX8(0xA2u, p[1]);
  TEST_ASSERT_EQUAL_HEX8(0x27u, p[2]);
  TEST_ASSERT_EQUAL_HEX8(0x10u, p[3]);
  TEST_ASSERT_EQUAL_HEX8(0x00u, p[4]);
  TEST_ASSERT_EQUAL_HEX8(0x1Bu, p[5]);

  StatusMsg s;
  fillStatus(&s);
  TEST_ASSERT_TRUE(packStatus(s, p));
  TEST_ASSERT_EQUAL_HEX8(0x01u, p[0]);
  TEST_ASSERT_EQUAL_HEX8(0x02u, p[1]);
  TEST_ASSERT_EQUAL_HEX8(0x00u, p[2]);   // uptime_ms 0x00010203
  TEST_ASSERT_EQUAL_HEX8(0x01u, p[3]);
  TEST_ASSERT_EQUAL_HEX8(0x02u, p[4]);
  TEST_ASSERT_EQUAL_HEX8(0x03u, p[5]);
  TEST_ASSERT_EQUAL_HEX8(0x11u, p[6]);   // frames_ok
  TEST_ASSERT_EQUAL_HEX8(0x22u, p[7]);
  TEST_ASSERT_EQUAL_HEX8(0x33u, p[8]);   // frames_dropped
  TEST_ASSERT_EQUAL_HEX8(0x44u, p[9]);
  TEST_ASSERT_EQUAL_HEX8(0x55u, p[10]);  // crc_err
  TEST_ASSERT_EQUAL_HEX8(0x66u, p[11]);
  TEST_ASSERT_EQUAL_HEX8(0x77u, p[12]);  // last_gap_ms
  TEST_ASSERT_EQUAL_HEX8(0x88u, p[13]);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)Face::Sport, p[14]);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)(kStFlagVerMismatch | kStFlagTempNoSource), p[15]);
  TEST_ASSERT_EQUAL_HEX8(0xEEu, p[16]);  // STATUS 恰好写 16 个字节,不多写一个

  EventMsg e;
  fillEvent(&e);
  TEST_ASSERT_TRUE(packEvent(e, p));
  TEST_ASSERT_EQUAL_HEX8((uint8_t)EvtId::FaceChange, p[0]);
  TEST_ASSERT_EQUAL_HEX8(0x12u, p[1]);
  TEST_ASSERT_EQUAL_HEX8(0x34u, p[2]);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)Face::City, p[3]);
}

// 往返：每个结构体的每个字段都要一模一样地回来
static void test_link_msg_roundtrip_all(void) {
  uint8_t p[kStatusLen];

  HelloMsg h, h2;
  fillHello(&h);
  TEST_ASSERT_TRUE(packHello(h, p));
  TEST_ASSERT_TRUE(unpackHello(p, kHelloLen, &h2));
  TEST_ASSERT_EQUAL_HEX16(h.fw_ver, h2.fw_ver);
  TEST_ASSERT_EQUAL_HEX16(h.build_tag, h2.build_tag);
  TEST_ASSERT_EQUAL_HEX8(h.boot_reason, h2.boot_reason);

  TickMsg t, t2;
  fillTick(&t);
  TEST_ASSERT_TRUE(packTick(t, p));
  TEST_ASSERT_TRUE(unpackTick(p, kTickLen, &t2));
  TEST_ASSERT_EQUAL_HEX32(t.tick_ms, t2.tick_ms);
  TEST_ASSERT_EQUAL_HEX8(t.seq, t2.seq);

  DataMsg d, d2;
  fillData(&d);
  TEST_ASSERT_TRUE(packData(d, p));
  TEST_ASSERT_TRUE(unpackData(p, kDataLen, &d2));
  TEST_ASSERT_EQUAL_HEX16(d.rpm_raw, d2.rpm_raw);
  TEST_ASSERT_EQUAL_HEX8(d.speed_raw, d2.speed_raw);
  TEST_ASSERT_EQUAL_HEX8(d.coolant_raw, d2.coolant_raw);
  TEST_ASSERT_EQUAL_HEX8(d.intake_raw, d2.intake_raw);
  TEST_ASSERT_EQUAL_HEX8(d.flags, d2.flags);

  StatusMsg s, s2;
  fillStatus(&s);
  TEST_ASSERT_TRUE(packStatus(s, p));
  TEST_ASSERT_TRUE(unpackStatus(p, kStatusLen, &s2));
  TEST_ASSERT_EQUAL_HEX16(s.fw_ver, s2.fw_ver);
  TEST_ASSERT_EQUAL_HEX32(s.uptime_ms, s2.uptime_ms);
  TEST_ASSERT_EQUAL_HEX16(s.frames_ok, s2.frames_ok);
  TEST_ASSERT_EQUAL_HEX16(s.frames_dropped, s2.frames_dropped);
  TEST_ASSERT_EQUAL_HEX16(s.crc_err, s2.crc_err);
  TEST_ASSERT_EQUAL_HEX16(s.last_gap_ms, s2.last_gap_ms);
  TEST_ASSERT_EQUAL_HEX8(s.left_face, s2.left_face);
  TEST_ASSERT_EQUAL_HEX8(s.flags, s2.flags);

  EventMsg e, e2;
  fillEvent(&e);
  TEST_ASSERT_TRUE(packEvent(e, p));
  TEST_ASSERT_TRUE(unpackEvent(p, kEventLen, &e2));
  TEST_ASSERT_EQUAL_HEX8(e.evt_id, e2.evt_id);
  TEST_ASSERT_EQUAL_HEX16(e.value, e2.value);
  TEST_ASSERT_EQUAL_HEX8(e.face, e2.face);

  // 极端值也不许被截断/回绕
  TickMsg big;
  big.tick_ms = 0xFFFFFFFFu;   // 32 位单调毫秒的回绕点（49.7 天）
  big.seq = 0xFFu;
  TEST_ASSERT_TRUE(packTick(big, p));
  TEST_ASSERT_TRUE(unpackTick(p, kTickLen, &t2));
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, t2.tick_ms);
  TEST_ASSERT_EQUAL_HEX8(0xFFu, t2.seq);

  StatusMsg maxs;
  maxs.fw_ver = 0xFFFFu;
  maxs.uptime_ms = 0xFFFFFFFFu;
  maxs.frames_ok = 0xFFFFu;
  maxs.frames_dropped = 0xFFFFu;
  maxs.crc_err = 0xFFFFu;
  maxs.last_gap_ms = 0xFFFFu;
  maxs.left_face = 0xFFu;
  maxs.flags = 0xFFu;
  TEST_ASSERT_TRUE(packStatus(maxs, p));
  TEST_ASSERT_TRUE(unpackStatus(p, kStatusLen, &s2));
  TEST_ASSERT_EQUAL_HEX16(0xFFFFu, s2.fw_ver);
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, s2.uptime_ms);
  TEST_ASSERT_EQUAL_HEX16(0xFFFFu, s2.frames_ok);
  TEST_ASSERT_EQUAL_HEX16(0xFFFFu, s2.frames_dropped);
  TEST_ASSERT_EQUAL_HEX16(0xFFFFu, s2.crc_err);
  TEST_ASSERT_EQUAL_HEX16(0xFFFFu, s2.last_gap_ms);
  TEST_ASSERT_EQUAL_HEX8(0xFFu, s2.left_face);
  TEST_ASSERT_EQUAL_HEX8(0xFFu, s2.flags);
}

// §2 的帧长清单：TICK 12 B、HELLO 12 B、DATA 13 B、STATUS 23 B
// ★ 清单里**没有 EVENT** —— 按同一个公式 7 + 4 = 11 B（契约缺口，见回报）。
static void test_link_msg_frame_lengths_match_contract(void) {
  uint8_t payload[kLenMax];
  memset(payload, 0, sizeof(payload));
  uint8_t frame[kParseBufBytes];

  TEST_ASSERT_EQUAL_UINT8(5u, kHelloLen);
  TEST_ASSERT_EQUAL_UINT8(5u, kTickLen);
  TEST_ASSERT_EQUAL_UINT8(6u, kDataLen);
  TEST_ASSERT_EQUAL_UINT8(16u, kStatusLen);
  TEST_ASSERT_EQUAL_UINT8(4u, kEventLen);

  TEST_ASSERT_EQUAL_UINT16(12u, encodeFrame((uint8_t)MsgType::Hello, payload, kHelloLen,
                                            kRoleMaster, frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_UINT16(12u, encodeFrame((uint8_t)MsgType::Tick, payload, kTickLen,
                                            kRoleMaster, frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_UINT16(13u, encodeFrame((uint8_t)MsgType::Data, payload, kDataLen,
                                            kRoleMaster, frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_UINT16(23u, encodeFrame((uint8_t)MsgType::Status, payload, kStatusLen,
                                            kRoleMaster, frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_UINT16(11u, encodeFrame((uint8_t)MsgType::Event, payload, kEventLen,
                                            kRoleMaster, frame, sizeof(frame)));

  // payloadLenForType 必须与上面这组长度一致（它是长度自检的唯一入口）
  TEST_ASSERT_EQUAL_UINT8(kHelloLen, payloadLenForType((uint8_t)MsgType::Hello));
  TEST_ASSERT_EQUAL_UINT8(kTickLen, payloadLenForType((uint8_t)MsgType::Tick));
  TEST_ASSERT_EQUAL_UINT8(kDataLen, payloadLenForType((uint8_t)MsgType::Data));
  TEST_ASSERT_EQUAL_UINT8(kStatusLen, payloadLenForType((uint8_t)MsgType::Status));
  TEST_ASSERT_EQUAL_UINT8(kEventLen, payloadLenForType((uint8_t)MsgType::Event));
}

// ------------------------------------------------------------
// DATA.flags：4 个字段 × 2 位（口径见 link_msg.h 文件头 ②）
// ------------------------------------------------------------
static void test_link_msg_data_flags_two_bits_per_field(void) {
  // 顺序 rpm(bit7..6) / speed(bit5..4) / coolant(bit3..2) / intake(bit1..0)
  TEST_ASSERT_EQUAL_HEX8(0xE4u, dataFlagsPack(Src::Van, Src::Obd, Src::Sim, Src::None));
  TEST_ASSERT_EQUAL_HEX8(0x00u, dataFlagsPack(Src::None, Src::None, Src::None, Src::None));
  TEST_ASSERT_EQUAL_HEX8(0xFFu, dataFlagsPack(Src::Van, Src::Van, Src::Van, Src::Van));

  // 256 个 flags 值全枚举：取值必须落在对应的 2 位上，且能原样拼回去
  for (uint16_t v = 0; v < 256u; ++v) {
    const uint8_t f = (uint8_t)v;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((f >> 6) & 0x03u), (uint8_t)dataFlagsGet(f, kFieldRpm));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((f >> 4) & 0x03u), (uint8_t)dataFlagsGet(f, kFieldSpeed));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((f >> 2) & 0x03u), (uint8_t)dataFlagsGet(f, kFieldCoolant));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(f & 0x03u), (uint8_t)dataFlagsGet(f, kFieldIntake));
    TEST_ASSERT_EQUAL_HEX8(f, dataFlagsPack(dataFlagsGet(f, kFieldRpm),
                                            dataFlagsGet(f, kFieldSpeed),
                                            dataFlagsGet(f, kFieldCoolant),
                                            dataFlagsGet(f, kFieldIntake)));
  }
  // 越界字段下标：不读越界的内存，给 None
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Src::None, (uint8_t)dataFlagsGet(0xFFu, 4u));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)Src::None, (uint8_t)dataFlagsGet(0xFFu, 255u));
}

// ------------------------------------------------------------
// 量纲换算与钳制
// ------------------------------------------------------------
static void test_link_msg_scaling_and_clamping(void) {
  const float nan = std::numeric_limits<float>::quiet_NaN();

  // 转速：raw = rpm × 8（与 VanSource::kRpmScale = 0.125 配套）
  TEST_ASSERT_EQUAL_UINT16(7200u, rpmToRaw(900.0f));        // 实测怠速锚点
  TEST_ASSERT_EQUAL_UINT16(0u, rpmToRaw(0.0f));
  TEST_ASSERT_EQUAL_UINT16(0u, rpmToRaw(-120.0f));          // 负值不许回绕成 65535
  TEST_ASSERT_EQUAL_UINT16(65535u, rpmToRaw(8191.875f));    // 量程上限
  TEST_ASSERT_EQUAL_UINT16(65535u, rpmToRaw(20000.0f));     // 越界钳到最大
  TEST_ASSERT_EQUAL_UINT16(0u, rpmToRaw(nan));              // NaN 钳 0
  TEST_ASSERT_EQUAL_FLOAT(0.125f * 65535.0f, rawToRpm(65535u));

  // 车速：1 计数 = 2.56 km/h（2026-09-22 实测定标）
  TEST_ASSERT_EQUAL_UINT8(39u, speedToRaw(100.0f));         // 100/2.56 = 39.06 → 39
  TEST_ASSERT_EQUAL_UINT8(0u, speedToRaw(0.0f));
  TEST_ASSERT_EQUAL_UINT8(0u, speedToRaw(-3.0f));
  TEST_ASSERT_EQUAL_UINT8(255u, speedToRaw(652.8f));        // 恰好量程上限
  TEST_ASSERT_EQUAL_UINT8(255u, speedToRaw(1000.0f));       // 越界钳到最大
  TEST_ASSERT_EQUAL_UINT8(0u, speedToRaw(nan));
  TEST_ASSERT_EQUAL_FLOAT(2.56f * 255.0f, rawToSpeedKmh(255u));

  // 温度：raw = ℃ + 40（0 → -40℃、255 → 215℃）
  TEST_ASSERT_EQUAL_UINT8(40u, tempToRaw(0.0f));
  TEST_ASSERT_EQUAL_UINT8(130u, tempToRaw(90.0f));
  TEST_ASSERT_EQUAL_UINT8(0u, tempToRaw(-40.0f));           // 量程下限
  TEST_ASSERT_EQUAL_UINT8(0u, tempToRaw(-100.0f));          // 越界钳到最小
  TEST_ASSERT_EQUAL_UINT8(255u, tempToRaw(215.0f));         // 量程上限
  TEST_ASSERT_EQUAL_UINT8(255u, tempToRaw(500.0f));         // 越界钳到最大
  TEST_ASSERT_EQUAL_UINT8(0u, tempToRaw(nan));
  TEST_ASSERT_EQUAL_FLOAT(-40.0f, rawToTempC(0u));
  TEST_ASSERT_EQUAL_FLOAT(215.0f, rawToTempC(255u));
}

// raw ↔ 物理量必须能原样往返（换算写反了就会在这儿露出来）
static void test_link_msg_scaling_roundtrip(void) {
  for (uint32_t raw = 0; raw <= 65535u; raw += 997u) {
    TEST_ASSERT_EQUAL_UINT16((uint16_t)raw, rpmToRaw(rawToRpm((uint16_t)raw)));
  }
  TEST_ASSERT_EQUAL_UINT16(65535u, rpmToRaw(rawToRpm(65535u)));
  for (uint32_t raw = 0; raw <= 255u; ++raw) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)raw, speedToRaw(rawToSpeedKmh((uint8_t)raw)));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)raw, tempToRaw(rawToTempC((uint8_t)raw)));
  }
}

// ------------------------------------------------------------
// STATUS.flags 的位号（★ 契约只列了含义、没给位号，见回报）+ 槽位下标
// ------------------------------------------------------------
static void test_link_msg_status_flags_and_face_slots(void) {
  TEST_ASSERT_EQUAL_HEX8(0x01u, kStFlagVerMismatch);
  TEST_ASSERT_EQUAL_HEX8(0x02u, kStFlagRoleConflict);
  TEST_ASSERT_EQUAL_HEX8(0x04u, kStFlagNoData);
  TEST_ASSERT_EQUAL_HEX8(0x08u, kStFlagTempNoSource);
  TEST_ASSERT_EQUAL_HEX8(0x0Fu, (uint8_t)(kStFlagVerMismatch | kStFlagRoleConflict |
                                          kStFlagNoData | kStFlagTempNoSource));

  // left_face / EVENT.face 用的是 expression.h 的 Face 槽位下标（0..6）
  TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)Face::Idle);
  TEST_ASSERT_EQUAL_UINT8(1u, (uint8_t)Face::Cruise);
  TEST_ASSERT_EQUAL_UINT8(2u, (uint8_t)Face::Sport);
  TEST_ASSERT_EQUAL_UINT8(3u, (uint8_t)Face::Redline);
  TEST_ASSERT_EQUAL_UINT8(4u, (uint8_t)Face::Overspeed);
  TEST_ASSERT_EQUAL_UINT8(5u, (uint8_t)Face::High);
  TEST_ASSERT_EQUAL_UINT8(6u, (uint8_t)Face::City);
  // 一个字节装得下（Face 是 uint8_t 枚举，7 个槽位）
  TEST_ASSERT_TRUE((uint8_t)Face::Count <= 255u);
}

// ------------------------------------------------------------
// EVENT 的事件号段（§3）
// ------------------------------------------------------------
static void test_link_msg_event_ids(void) {
  TEST_ASSERT_EQUAL_HEX8(0x01u, (uint8_t)EvtId::RedlineEdge);
  TEST_ASSERT_EQUAL_HEX8(0x02u, (uint8_t)EvtId::OverspeedEdge);
  TEST_ASSERT_EQUAL_HEX8(0x03u, (uint8_t)EvtId::FaceChange);
  TEST_ASSERT_EQUAL_HEX8(0x04u, (uint8_t)EvtId::SourceChange);
  TEST_ASSERT_EQUAL_HEX8(0x05u, (uint8_t)EvtId::SweepDone);
  TEST_ASSERT_EQUAL_HEX8(0x06u, (uint8_t)EvtId::ModeChange);   // 保留号段：v1 不做，不许挪作他用

  TEST_ASSERT_TRUE(evtIdKnown(0x01u));
  TEST_ASSERT_TRUE(evtIdKnown(0x05u));
  TEST_ASSERT_FALSE(evtIdKnown(0x06u));   // v1 不做 ⇒ 不算"已知事件"
  TEST_ASSERT_FALSE(evtIdKnown(0x00u));
  TEST_ASSERT_FALSE(evtIdKnown(0x07u));
  TEST_ASSERT_FALSE(evtIdKnown(0xFFu));

  // ★ 未知事件号**照样解出来**（不丢帧）：§2 的"未知就丢"是给 TYPE 的规矩，
  //   把未知 evt_id 丢掉会破坏"次版本只加东西"的向前兼容。要不要记日志由调用方判。
  const uint8_t p[kEventLen] = {0x7Eu, 0x12u, 0x34u, (uint8_t)Face::Redline};
  EventMsg e;
  TEST_ASSERT_TRUE(unpackEvent(p, kEventLen, &e));
  TEST_ASSERT_EQUAL_HEX8(0x7Eu, e.evt_id);
  TEST_ASSERT_EQUAL_HEX16(0x1234u, e.value);
  TEST_ASSERT_EQUAL_HEX8((uint8_t)Face::Redline, e.face);
}

// ------------------------------------------------------------
// 与帧层连起来：§2 的次版本规矩（"已知 TYPE 的载荷只许加尾巴"）
// ------------------------------------------------------------
static void test_link_msg_prefix_compat_with_tail(void) {
  // 造一个 DATA 帧，但载荷比 v1 的 6 字节多 4 字节尾巴（LEN = 10，仍在 v1 的 5..16 内）：
  // 老固件必须**认得前半截**。
  uint8_t payload[kDataLen + 4];
  DataMsg d, d2;
  fillData(&d);
  TEST_ASSERT_TRUE(packData(d, payload));
  payload[kDataLen + 0] = 0xAAu;   // v2 才有的尾巴
  payload[kDataLen + 1] = 0xBBu;
  payload[kDataLen + 2] = 0xCCu;
  payload[kDataLen + 3] = 0xDDu;

  uint8_t frame[kParseBufBytes];
  const uint16_t n = encodeFrame((uint8_t)MsgType::Data, payload, kDataLen + 4u, kRoleMaster,
                                 frame, sizeof(frame));
  TEST_ASSERT_EQUAL_UINT16(17u, n);

  Frame f;
  TEST_ASSERT_EQUAL_STRING("ok", decodeErrName(decodeFrame(frame, n, &f)));
  TEST_ASSERT_EQUAL_UINT8(kDataLen + 4u, f.len);
  TEST_ASSERT_TRUE(unpackData(f.payload, f.len, &d2));
  TEST_ASSERT_EQUAL_HEX16(d.rpm_raw, d2.rpm_raw);
  TEST_ASSERT_EQUAL_HEX8(d.speed_raw, d2.speed_raw);
  TEST_ASSERT_EQUAL_HEX8(d.coolant_raw, d2.coolant_raw);
  TEST_ASSERT_EQUAL_HEX8(d.intake_raw, d2.intake_raw);
  TEST_ASSERT_EQUAL_HEX8(d.flags, d2.flags);

  // 尾巴再长也一样，只要帧层的 LEN 还在 5..16
  TEST_ASSERT_TRUE(unpackData(f.payload, kLenMax, &d2));
  TEST_ASSERT_EQUAL_HEX16(d.rpm_raw, d2.rpm_raw);
  // 短了就不行（前缀都不全）
  TEST_ASSERT_FALSE(unpackData(f.payload, kDataLen - 1u, &d2));
}

// 载荷长度不足 / 空指针：一律 false（不许半解）
static void test_link_msg_unpack_rejects_short_and_null(void) {
  uint8_t p[kStatusLen];
  memset(p, 0, sizeof(p));
  HelloMsg h;
  TickMsg t;
  DataMsg d;
  StatusMsg s;
  EventMsg e;
  TEST_ASSERT_FALSE(unpackHello(p, kHelloLen - 1u, &h));
  TEST_ASSERT_FALSE(unpackTick(p, kTickLen - 1u, &t));
  TEST_ASSERT_FALSE(unpackData(p, kDataLen - 1u, &d));
  TEST_ASSERT_FALSE(unpackStatus(p, kStatusLen - 1u, &s));
  TEST_ASSERT_FALSE(unpackEvent(p, kEventLen - 1u, &e));
  TEST_ASSERT_FALSE(unpackHello(p, 0u, &h));
  TEST_ASSERT_FALSE(unpackStatus(p, kStatusLen, nullptr));
  TEST_ASSERT_FALSE(unpackEvent(nullptr, kEventLen, &e));
  TEST_ASSERT_FALSE(packHello(h, nullptr));
  TEST_ASSERT_FALSE(packTick(t, nullptr));
  TEST_ASSERT_FALSE(packData(d, nullptr));
  TEST_ASSERT_FALSE(packStatus(s, nullptr));
  TEST_ASSERT_FALSE(packEvent(e, nullptr));
  // 够长就必须成功（边界）
  TEST_ASSERT_TRUE(unpackHello(p, kHelloLen, &h));
  TEST_ASSERT_TRUE(unpackTick(p, kTickLen, &t));
  TEST_ASSERT_TRUE(unpackData(p, kDataLen, &d));
  TEST_ASSERT_TRUE(unpackStatus(p, kStatusLen, &s));
  TEST_ASSERT_TRUE(unpackEvent(p, kEventLen, &e));
}

// FieldSource ↔ 2 位编码的数值对账（编译期那份 static_assert 的运行期版本，
// 让这条对账在用例表里也看得见）
static void test_link_msg_source_bits_match_field_source(void) {
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::None, (uint8_t)Src::None);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Sim, (uint8_t)Src::Sim);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Obd, (uint8_t)Src::Obd);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Van, (uint8_t)Src::Van);
  // 一份"四个字段各来自不同源"的 DATA：flags 与逐字段取值必须自洽
  DataMsg m;
  m.flags = dataFlagsPack(Src::Van, Src::Van, Src::Obd, Src::Sim);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Van, (uint8_t)dataFlagsGet(m.flags, kFieldRpm));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Van, (uint8_t)dataFlagsGet(m.flags, kFieldSpeed));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Obd, (uint8_t)dataFlagsGet(m.flags, kFieldCoolant));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)FieldSource::Sim, (uint8_t)dataFlagsGet(m.flags, kFieldIntake));
}

// ------------------------------------------------------------
// VANRAW 0x21（2026-09-27）：**变长**载荷的逐字节布局
// ------------------------------------------------------------
// 这一族与其它五个 TYPE 的**根本区别**：长度不固定（= 4 + 那一帧自己的数据长度）。
// 所以用例要钉住的不是"某一串字节"，而是**边界与自洽**：
//   · 头 4 字节的布局（iden 大端 / dlen / hdr 的三段位）；
//   · **能装下 0x4FC 的 11 字节**（灯位帧 —— 这是 `kVanRawMaxData` 取 12 的理由）；
//   · 装不下 VIN 的 17 字节（**设计限制**，要显式钉住它不静默截断）；
//   · unpack 必须拒绝"dlen 与 len 对不上"的载荷（半截帧 / 多尾巴）。
static void fillVanRaw(VanRawMsg* m, uint8_t dlen) {
  m->iden = 0x824u;
  m->cmd = 0x8u;
  m->ack = false;
  m->fcs_ok = true;
  m->len = dlen;
  for (uint8_t i = 0; i < dlen; ++i) m->data[i] = (uint8_t)(0xA0u + i);
}

static void test_link_msg_vanraw_byte_layout(void) {
  VanRawMsg m;
  fillVanRaw(&m, 7u);                       // 车速帧的真实长度（van_source.h）
  m.data[0] = 0x18; m.data[1] = 0xF8;       // 转速 0x18F8 = 6392 ⇒ 799.0 rpm
  m.data[2] = 0x27;                         // 车速 39 计数 ⇒ 99.84 km/h
  uint8_t p[kLenMax];
  memset(p, 0xEE, sizeof(p));
  const uint8_t n = packVanRaw(m, p);
  TEST_ASSERT_EQUAL_UINT8(11u, n);          // 4 + 7
  TEST_ASSERT_EQUAL_HEX8(0x08u, p[0]);      // iden 高字节（大端）
  TEST_ASSERT_EQUAL_HEX8(0x24u, p[1]);      // iden 低字节
  TEST_ASSERT_EQUAL_HEX8(7u, p[2]);         // dlen
  TEST_ASSERT_EQUAL_HEX8(0x28u, p[3]);      // cmd=8 | fcs_ok=1(fcs 位 0x20)
  TEST_ASSERT_EQUAL_HEX8(0x18u, p[4]);
  TEST_ASSERT_EQUAL_HEX8(0xF8u, p[5]);
  TEST_ASSERT_EQUAL_HEX8(0x27u, p[6]);
  // 只写 n 个字节 —— 尾巴不许被碰
  TEST_ASSERT_EQUAL_HEX8(0xEEu, p[n]);
  // ack 位单独一位（与 fcs 分开）
  m.ack = true;
  TEST_ASSERT_EQUAL_UINT8(11u, packVanRaw(m, p));
  TEST_ASSERT_EQUAL_HEX8(0x38u, p[3]);      // cmd=8 | ack=0x10 | fcs=0x20
}

static void test_link_msg_vanraw_roundtrip_and_limits(void) {
  uint8_t p[kLenMax];
  // ① 装得下 0x4FC 的 11 字节（**这就是 kVanRawMaxData 取 12 的理由**）
  VanRawMsg m;
  fillVanRaw(&m, 11u);
  const uint8_t n11 = packVanRaw(m, p);
  TEST_ASSERT_EQUAL_UINT8(15u, n11);                // 4 + 11 = 15（≤ kLenMax 16 ✓）
  TEST_ASSERT_TRUE(n11 <= kLenMax);
  VanRawMsg back;
  TEST_ASSERT_TRUE(unpackVanRaw(p, n11, &back));
  TEST_ASSERT_EQUAL_HEX16(m.iden, back.iden);
  TEST_ASSERT_EQUAL_HEX8(11u, back.len);
  TEST_ASSERT_EQUAL_HEX8(m.cmd, back.cmd);
  TEST_ASSERT_TRUE(back.fcs_ok);
  for (uint8_t i = 0; i < 11u; ++i) TEST_ASSERT_EQUAL_HEX8(m.data[i], back.data[i]);

  // ② 满格（12 字节数据 ⇒ 载荷正好 16 = kLenMax）
  fillVanRaw(&m, kVanRawMaxData);
  TEST_ASSERT_EQUAL_UINT8(kLenMax, packVanRaw(m, p));
  TEST_ASSERT_TRUE(unpackVanRaw(p, kLenMax, &back));
  TEST_ASSERT_EQUAL_HEX8(kVanRawMaxData, back.len);

  // ③ 装不下 VIN 的 17 字节：**一个字节都不写**（绝不截断）
  fillVanRaw(&m, 17u);
  memset(p, 0xEE, sizeof(p));
  TEST_ASSERT_EQUAL_UINT8(0u, packVanRaw(m, p));
  TEST_ASSERT_EQUAL_HEX8(0xEEu, p[0]);
  TEST_ASSERT_EQUAL_UINT8(0u, packVanRaw(m, nullptr));

  // ④ 空数据（dlen = 0 ⇒ 载荷 4 字节，正好是帧层 LEN 下限）
  fillVanRaw(&m, 0u);
  TEST_ASSERT_EQUAL_UINT8(kVanRawHdrLen, packVanRaw(m, p));
  TEST_ASSERT_EQUAL_UINT8(kLenMin, packVanRaw(m, p));
  TEST_ASSERT_TRUE(unpackVanRaw(p, kVanRawHdrLen, &back));
  TEST_ASSERT_EQUAL_HEX8(0u, back.len);
}

// unpack 的判据是"**载荷里写的** dlen 必须与给进来的 len 对得上"：
// 半截载荷（len 小于 dlen+4）与多尾巴（len 大于 dlen+4）都必须是 false ——
// 前者会解出一个数据不全的帧（VanSource 会据此算出错误的转速/车速），
// 后者会把尾巴静默当成数据。
static void test_link_msg_vanraw_unpack_rejects_mismatched_len(void) {
  VanRawMsg m;
  fillVanRaw(&m, 7u);
  uint8_t p[kLenMax];
  const uint8_t n = packVanRaw(m, p);
  VanRawMsg back;
  TEST_ASSERT_TRUE(unpackVanRaw(p, n, &back));
  TEST_ASSERT_FALSE(unpackVanRaw(p, (uint8_t)(n - 1u), &back));   // 半截
  TEST_ASSERT_FALSE(unpackVanRaw(p, (uint8_t)(n + 1u), &back));   // 多一个尾巴
  TEST_ASSERT_FALSE(unpackVanRaw(p, kLenMax, &back));             // 多更多
  TEST_ASSERT_FALSE(unpackVanRaw(p, kVanRawHdrLen - 1u, &back));  // 连头都不全
  TEST_ASSERT_FALSE(unpackVanRaw(p, n, nullptr));
  TEST_ASSERT_FALSE(unpackVanRaw(nullptr, n, &back));
  // dlen 越界（> kVanRawMaxData）即使 len 对得上也必须拒绝
  uint8_t bad[kLenMax];
  memset(bad, 0, sizeof(bad));
  bad[2] = (uint8_t)(kVanRawMaxData + 1u);
  TEST_ASSERT_FALSE(unpackVanRaw(bad, (uint8_t)(kVanRawHdrLen + kVanRawMaxData + 1u), &back));
  // 载荷头 3 字节的位掩码：cmd 只有低 4 位有效（高位是 ack/fcs）
  uint8_t hdr[kVanRawHdrLen];
  memset(hdr, 0, sizeof(hdr));
  hdr[2] = 0u;
  hdr[3] = 0xFFu;
  TEST_ASSERT_TRUE(unpackVanRaw(hdr, kVanRawHdrLen, &back));
  TEST_ASSERT_EQUAL_HEX8(kVanRawCmdMask, back.cmd);   // 0x0F，不是 0xFF
  TEST_ASSERT_TRUE(back.ack);
  TEST_ASSERT_TRUE(back.fcs_ok);
}

// TYPE 号段的账：`0x21` 是 VANRAW，`0x50` 仍然是 §3 留给 v2 的保留段（**不许占**）。
static void test_link_msg_vanraw_type_number(void) {
  TEST_ASSERT_EQUAL_HEX8(0x21u, (uint8_t)MsgType::VanRaw);
  TEST_ASSERT_EQUAL_HEX8(0x20u, (uint8_t)MsgType::Data);   // 与 DATA 相邻（同一生产者）
  TEST_ASSERT_TRUE(typeKnown((uint8_t)MsgType::VanRaw));
  TEST_ASSERT_TRUE(typeKnown((uint8_t)MsgType::Data));
  TEST_ASSERT_FALSE(typeKnown(0x50u));    // v2 保留段：本层仍然不认
  TEST_ASSERT_FALSE(typeKnown(0x22u));    // 空号
  TEST_ASSERT_EQUAL_STRING("VANRAW", msgTypeName((uint8_t)MsgType::VanRaw));
  // 变长 TYPE 在 payloadLenForType() 里报 0（= 变长或不存在，**不是**"期望 0 字节"）
  TEST_ASSERT_EQUAL_UINT8(0u, payloadLenForType((uint8_t)MsgType::VanRaw));
}

void register_link_msg_tests(void) {
  RUN_TEST(test_link_msg_byte_layout_and_endianness);
  RUN_TEST(test_link_msg_roundtrip_all);
  RUN_TEST(test_link_msg_frame_lengths_match_contract);
  RUN_TEST(test_link_msg_data_flags_two_bits_per_field);
  RUN_TEST(test_link_msg_scaling_and_clamping);
  RUN_TEST(test_link_msg_scaling_roundtrip);
  RUN_TEST(test_link_msg_status_flags_and_face_slots);
  RUN_TEST(test_link_msg_event_ids);
  RUN_TEST(test_link_msg_prefix_compat_with_tail);
  RUN_TEST(test_link_msg_unpack_rejects_short_and_null);
  RUN_TEST(test_link_msg_source_bits_match_field_source);
  RUN_TEST(test_link_msg_vanraw_byte_layout);
  RUN_TEST(test_link_msg_vanraw_roundtrip_and_limits);
  RUN_TEST(test_link_msg_vanraw_unpack_rejects_mismatched_len);
  RUN_TEST(test_link_msg_vanraw_type_number);
}

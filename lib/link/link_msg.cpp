// 双板链路协议 v1 —— 消息载荷实现（§3）。纯逻辑，逐字节按契约表。
//
// ★ 多字节字段一律大端（口径见 link_msg.h 文件头 ①）：putU16/getU16/putU32/getU32
//   是唯一入口，别在别处手写移位 —— 字节序写错不会有任何编译期信号，
//   只会表现成"两板的 tick_ms 差了 16777216 ms"。
#include "link_msg.h"

namespace dashlink {

namespace {

inline void putU16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFFu);
}
inline uint16_t getU16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
inline void putU32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)((v >> 16) & 0xFFu);
  p[2] = (uint8_t)((v >> 8) & 0xFFu);
  p[3] = (uint8_t)(v & 0xFFu);
}
inline uint32_t getU32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

}  // namespace

uint8_t payloadLenForType(uint8_t type) {
  switch (type) {
    case (uint8_t)MsgType::Hello:  return kHelloLen;
    case (uint8_t)MsgType::Tick:   return kTickLen;
    case (uint8_t)MsgType::Data:   return kDataLen;
    case (uint8_t)MsgType::Status: return kStatusLen;
    case (uint8_t)MsgType::Event:  return kEventLen;
    default:                       return 0u;
  }
}

// ------------------------------------------------------------
// HELLO 0x01：fw_ver u16 | build_tag u16 | boot_reason u8
// ------------------------------------------------------------
bool packHello(const HelloMsg& m, uint8_t* out) {
  if (out == nullptr) return false;
  putU16(out + 0, m.fw_ver);
  putU16(out + 2, m.build_tag);
  out[4] = m.boot_reason;
  return true;
}

bool unpackHello(const uint8_t* p, uint8_t len, HelloMsg* out) {
  if (p == nullptr || out == nullptr || len < kHelloLen) return false;
  out->fw_ver      = getU16(p + 0);
  out->build_tag   = getU16(p + 2);
  out->boot_reason = p[4];
  return true;
}

// ------------------------------------------------------------
// TICK 0x10：tick_ms u32 | seq u8
// ------------------------------------------------------------
bool packTick(const TickMsg& m, uint8_t* out) {
  if (out == nullptr) return false;
  putU32(out, m.tick_ms);
  out[4] = m.seq;
  return true;
}

bool unpackTick(const uint8_t* p, uint8_t len, TickMsg* out) {
  if (p == nullptr || out == nullptr || len < kTickLen) return false;
  out->tick_ms = getU32(p);
  out->seq     = p[4];
  return true;
}

// ------------------------------------------------------------
// DATA 0x20：rpm_raw u16 BE | speed_raw u8 | coolant_raw u8 | intake_raw u8 | flags u8
// ------------------------------------------------------------
bool packData(const DataMsg& m, uint8_t* out) {
  if (out == nullptr) return false;
  putU16(out + 0, m.rpm_raw);
  out[2] = m.speed_raw;
  out[3] = m.coolant_raw;
  out[4] = m.intake_raw;
  out[5] = m.flags;
  return true;
}

bool unpackData(const uint8_t* p, uint8_t len, DataMsg* out) {
  if (p == nullptr || out == nullptr || len < kDataLen) return false;
  out->rpm_raw     = getU16(p + 0);
  out->speed_raw   = p[2];
  out->coolant_raw = p[3];
  out->intake_raw  = p[4];
  out->flags       = p[5];
  return true;
}

// ------------------------------------------------------------
// STATUS 0x30：fw_ver | uptime_ms u32 | frames_ok | frames_dropped |
//              crc_err | last_gap_ms | left_face | flags   （共 16 B）
// ------------------------------------------------------------
bool packStatus(const StatusMsg& m, uint8_t* out) {
  if (out == nullptr) return false;
  putU16(out + 0, m.fw_ver);
  putU32(out + 2, m.uptime_ms);
  putU16(out + 6, m.frames_ok);
  putU16(out + 8, m.frames_dropped);
  putU16(out + 10, m.crc_err);
  putU16(out + 12, m.last_gap_ms);
  out[14] = m.left_face;
  out[15] = m.flags;
  return true;
}

bool unpackStatus(const uint8_t* p, uint8_t len, StatusMsg* out) {
  if (p == nullptr || out == nullptr || len < kStatusLen) return false;
  out->fw_ver         = getU16(p + 0);
  out->uptime_ms      = getU32(p + 2);
  out->frames_ok      = getU16(p + 6);
  out->frames_dropped = getU16(p + 8);
  out->crc_err        = getU16(p + 10);
  out->last_gap_ms    = getU16(p + 12);
  out->left_face      = p[14];
  out->flags          = p[15];
  return true;
}

// ------------------------------------------------------------
// EVENT 0x40：evt_id u8 | value u16 | face u8
// ------------------------------------------------------------
bool packEvent(const EventMsg& m, uint8_t* out) {
  if (out == nullptr) return false;
  out[0] = m.evt_id;
  putU16(out + 1, m.value);
  out[3] = m.face;
  return true;
}

bool unpackEvent(const uint8_t* p, uint8_t len, EventMsg* out) {
  if (p == nullptr || out == nullptr || len < kEventLen) return false;
  out->evt_id = p[0];
  out->value  = getU16(p + 1);
  out->face   = p[3];
  return true;
}

// ------------------------------------------------------------
// 字段来源的 2 位编码（口径见 link_msg.h 文件头 ②）
// ------------------------------------------------------------
uint8_t dataFlagsPack(Src rpm, Src speed, Src coolant, Src intake) {
  return (uint8_t)(((uint8_t)rpm << 6) | ((uint8_t)speed << 4) |
                   ((uint8_t)coolant << 2) | (uint8_t)intake);
}

Src dataFlagsGet(uint8_t flags, uint8_t field) {
  if (field > kFieldIntake) return Src::None;
  return (Src)((flags >> (6u - 2u * field)) & 0x03u);
}

// ------------------------------------------------------------
// 量纲换算 + 钳制（越界不许回绕）
// ------------------------------------------------------------
uint16_t rpmToRaw(float rpm) {
  if (!(rpm > 0.0f)) return 0u;                    // 0 / 负值 / NaN
  const float v = rpm * kRpmCountsPerRpm;
  if (v >= 65535.0f) return 65535u;                // 8191.875 rpm 以上封顶
  return (uint16_t)(v + 0.5f);
}

float rawToRpm(uint16_t raw) { return (float)raw * kRpmScale; }

uint8_t speedToRaw(float kmh) {
  if (!(kmh > 0.0f)) return 0u;
  const float v = kmh / kSpeedKmhPerCount;
  if (v >= 255.0f) return 255u;                    // 652.8 km/h 以上封顶
  return (uint8_t)(v + 0.5f);
}

float rawToSpeedKmh(uint8_t raw) { return (float)raw * kSpeedKmhPerCount; }

uint8_t tempToRaw(float celsius) {
  const float v = celsius + kTempOffsetC;
  if (!(v > 0.0f)) return 0u;                      // ≤ -40℃ / NaN
  if (v >= 255.0f) return 255u;                    // ≥ 215℃
  return (uint8_t)(v + 0.5f);
}

float rawToTempC(uint8_t raw) { return (float)raw - kTempOffsetC; }

}  // namespace dashlink

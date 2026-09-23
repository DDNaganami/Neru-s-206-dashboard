// 双板链路协议 v1 —— 帧层实现（§2）。纯逻辑：不碰 Arduino / 寄存器 / 堆。
//
// CRC 用**仓库已有的** van::crc15_van_iso()（lib/dashcore/van_wire.h）：
//   多项式 0x0F9D、初值 0x7FFF、输出取反、MSB-first —— 已在两份真实抓包上逐帧命中
//   （drive5min 17106/17106、sample 66/66），所以链路帧直接复用，不另写一份。
//   ★ 别换成 crc15()（0x4599 = CAN-15）：那不是本总线的 FCS，实测 0 命中。
//   ★ 也别引入 fcsFieldFromCrc() 那次左移 —— 那是 VAN 线上 16 位 FCS 字段的约定
//     （最低位是 EOD 的一半），链路帧的字段直接大端存 15 位值（§2 的「校验」）。
#include "link_frame.h"

#include "van_wire.h"   // van::crc15_van_iso()（-I lib/dashcore）

namespace dashlink {

uint16_t frameCrc(const uint8_t* frame, uint16_t n) {
  if (frame == nullptr || n < kOverhead) return 0u;
  // 覆盖 VER..载荷末尾 = frame[1 .. n-3]，共 n-3 个字节（n = 7 + LEN）：
  //   VER(1) + TYPE(1) + LEN(1) + ROLE(1) + LEN 个载荷 = 4 + LEN = n - 3
  //   不含 SYNC（frame[0]）、不含末尾 2 字节 CRC 自己 ⇒ n - 1 - kCrcBytes。
  // ★ 这里写错不会有任何编译期信号：encodeFrame / decodeFrame **两边同时错**时
  //   自造帧照样往返成功，只有"拿 crc15_van_iso 独立算一遍"的用例能抓住
  //   （test_link_frame_layout_and_roundtrip 就是这么抓出来的）。
  return van::crc15_van_iso(frame + kOffVer, (uint16_t)(n - kCrcBytes - 1u));
}

const char* msgTypeName(uint8_t type) {
  switch (type) {
    case (uint8_t)MsgType::Hello:  return "HELLO";
    case (uint8_t)MsgType::Tick:   return "TICK";
    case (uint8_t)MsgType::Data:   return "DATA";
    case (uint8_t)MsgType::Status: return "STATUS";
    case (uint8_t)MsgType::Event:  return "EVENT";
    default:                       return "unknown";
  }
}

const char* decodeErrName(DecodeErr e) {
  switch (e) {
    case DecodeErr::Ok:            return "ok";
    case DecodeErr::BadSync:       return "bad_sync";
    case DecodeErr::ShortFrame:    return "short";
    case DecodeErr::LenOutOfRange: return "len_range";
    case DecodeErr::LenMismatch:   return "len_mismatch";
    case DecodeErr::CrcError:      return "crc";
    case DecodeErr::UnknownType:   return "unknown_type";
    default:                       return "?";
  }
}

uint16_t encodeFrame(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t role,
                     uint8_t* out, uint16_t cap, uint8_t ver) {
  if (out == nullptr || payload == nullptr) return 0u;
  if (!lenInRange(len)) return 0u;          // 只发 v1 合法长度（5..16）
  const uint16_t n = frameBytesForLen(len);
  if (cap < n) return 0u;                   // 容量不够：不写半截帧

  out[kOffSync] = kSync;
  out[kOffVer]  = ver;
  out[kOffType] = type;
  out[kOffLen]  = len;
  out[kOffRole] = role;
  for (uint8_t i = 0; i < len; ++i) out[kOffPayload + i] = payload[i];

  // ★ 15 位值直接 2 字节大端：字段 == crc15_van_iso()，不做任何移位。
  //   bit15 恒 0，所以接收侧拿 16 位字段与 15 位值比，天然会把"被人左移过"
  //   或"bit15 被置位"的帧判成 CRC 错（用例 test_link_frame_crc_field_is_15bit）。
  const uint16_t crc = frameCrc(out, n);
  out[n - 2u] = (uint8_t)(crc >> 8);
  out[n - 1u] = (uint8_t)(crc & 0xFFu);
  return n;
}

DecodeErr decodeFrame(const uint8_t* buf, uint16_t n, Frame* out) {
  // ① 连最短的帧都装不下（空指针同此：没有可读的字节）
  if (buf == nullptr || out == nullptr) return DecodeErr::ShortFrame;
  if (n < kOverhead) return DecodeErr::ShortFrame;

  // ② 帧头对齐：SYNC 不进 CRC，只能在这儿判
  if (buf[kOffSync] != kSync) return DecodeErr::BadSync;

  const uint8_t len = buf[kOffLen];

  // ③ > 64：按坏帧丢，**不等载荷**（§2）。我们根本没有那么长的缓冲区，
  //    所以连 CRC 都算不了 —— 这一条必须在长度检查之前。
  if (len > kLenNoWaitAbove) return DecodeErr::LenOutOfRange;

  // ④ LEN 与实到字节数不符（半截帧 / 多给了字节）
  if (n != frameBytesForLen(len)) return DecodeErr::LenMismatch;

  // ⑤ 完整性：16 位字段（大端）必须逐位等于本地算的 15 位值
  const uint16_t field = (uint16_t)(((uint16_t)buf[n - 2u] << 8) | buf[n - 1u]);
  const uint16_t calc  = frameCrc(buf, n);
  if (field != calc) return DecodeErr::CrcError;

  // ⑥ CRC 过了才知道这确实是一帧对端发出来的东西：此时 LEN 越界
  //    = 对端发了 v1 认不了的长载荷（不是误码），计数同样进 bad_len。
  if (!lenInRange(len)) return DecodeErr::LenOutOfRange;

  // ⑦ 未知 TYPE 一律丢帧并计数（§2）；已知 TYPE 即使主版本不同也照解。
  const uint8_t type = buf[kOffType];
  if (!typeKnown(type)) return DecodeErr::UnknownType;

  out->ver   = buf[kOffVer];
  out->type  = type;
  out->len   = len;
  out->role  = buf[kOffRole];
  for (uint8_t i = 0; i < len; ++i) out->payload[i] = buf[kOffPayload + i];
  out->crc      = field;
  out->crc_calc = calc;
  // ⑧ 主版本不同：只告警，不丢帧、不降级（§2）。次版本不同连告警都没有。
  out->ver_mismatch = (uint8_t)(out->ver >> 4) != kVerMajor;
  return DecodeErr::Ok;
}

}  // namespace dashlink

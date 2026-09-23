#pragma once
#include <stdint.h>

// ============================================================
// 双板链路协议 v1 —— **帧层**（纯逻辑，无 Arduino / 寄存器 / 动态分配依赖）
//
// ★★ 命名空间为什么叫 `dashlink` 而不是 `link`（2026-09-23，实测踩到）
//   LVGL 在 `lv_obj.h` 的**全局作用域**里声明了一个函数：
//       int link(const char* src, const char* dst);   // 硬链接一个文件
//   于是"LVGL + 本模块"同时进一个翻译单元时，`namespace link` 会与那个函数同名
//   **冲突**（GCC 原文：`'namespace link { }' redeclared as different kind of
//   symbol` / `note: previous declaration 'int link(const char*, const char*)'`），
//   而这个组合是**必然**要出现的 —— 显示固件的 `src/main.cpp` 里两者都要 include。
//   ⇒ 改名一次、彻底躲开，比"以后每次 include 都得排序/隔离"稳。
//   ★ 改名**只动了命名空间**：所有类名、常量名、宏名、协议口径一个都没变。
//
// 契约：ARCHITECTURE.md「## 双板链路协议 v1 范围」的 §2（帧格式）。
// 本文件只做"字节 ↔ 帧结构"，不做消息语义（那是 link_msg.h）、
// 不做字节流分帧与重同步（那是 link_rx.h）、不碰任何引脚（§1.3 / §6）。
//
//   | 偏移 | 宽 | 字段    | 取值 / 含义
//   |    0 |  1 | SYNC    | 固定 0x5A
//   |    1 |  1 | VER     | 高 4 位主版本、低 4 位次版本；v1 = 0x10
//   |    2 |  1 | TYPE    | §3 的消息表
//   |    3 |  1 | LEN     | 载荷字节数；v1 合法 4..16（★ 见下，§2 原文是 5..16）；
//   |      |    |         | >64 按坏帧丢（不等载荷）
//   |    4 |  1 | ROLE    | 1 = 主板(右) / 0 = 从板(左)
//   |    5 |  LEN | PAYLOAD |
//   | 5+LEN |  2 | CRC     | 16 位字段大端承载 **15 位** CRC（bit15 恒 0）
//   帧长 = 7 + LEN（§2 列了 TICK 12 B、HELLO 12 B、DATA 13 B、STATUS 23 B；
//   ★ EVENT 按同一公式是 11 B —— §2 那份清单里漏了它，见回报）
//
// ★ 三处**别照搬 VAN 那一套**（§2 的「校验」小节明确点了这两条）：
//   ① CRC **直接 2 字节大端存 15 位值**，不做 fcsFieldFromCrc() 那次左移 ——
//      那个"最低位恒 0"是 VAN 线上 16 位 FCS 字段的约定（那一位是 EOD 的一半），
//      链路帧不背这个包袱。所以这里**没有** fcsFieldFromCrc 那一族换算函数。
//   ② CRC 只认 van::crc15_van_iso()（覆盖 VER..载荷末尾）。**别用 crc15()**
//      （0x4599 = CAN-15），头文件写明它不是本总线的 FCS、实测 0 命中。
//
// ★ CRC 覆盖范围 = 字节 1..(4+LEN)，**不含 SYNC、不含 CRC 自己**：
//   SYNC 只用于对齐 —— 它被噪声改掉时这一帧会因为"找不到帧头"被跳过
//   （由 link_rx.h 重新找下一个 SYNC 兜住），而不是被当成好帧收下。
//   这条不等于"SYNC 位错被 CRC 检出"：decodeFrame() 里 SYNC 是**独立**的一条
//   拒绝路径（DecodeErr::BadSync），CRC 对 SYNC 的改动**完全不敏感**。
//   两者各管一头，用例 test_link_frame_sync_outside_crc 把这条钉住。
//
// ★ 版本不匹配**不是**拒绝路径（§2 的「版本不匹配时的行为」）：
//   主版本不同 ⇒ 上报 + 告警（Frame::ver_mismatch），已知 TYPE 照 v1 继续解析，
//   只把**未知 TYPE / 越界 LEN** 丢帧并计数；**不断链、不降级**。
// ============================================================

namespace dashlink {

// ---- 字面量（唯一权威定义，别在别处抄数字） ----
static const uint8_t kSync     = 0x5Au;
static const uint8_t kVerMajor = 1u;
static const uint8_t kVerMinor = 0u;
static const uint8_t kVer      = 0x10u;   // (kVerMajor << 4) | kVerMinor

// §2 偏移表
static const uint8_t kOffSync    = 0u;
static const uint8_t kOffVer     = 1u;
static const uint8_t kOffType    = 2u;
static const uint8_t kOffLen     = 3u;
static const uint8_t kOffRole    = 4u;
static const uint8_t kOffPayload = 5u;

static const uint8_t kHeaderBytes = 5u;   // SYNC..ROLE
static const uint8_t kCrcBytes    = 2u;
static const uint8_t kOverhead    = kHeaderBytes + kCrcBytes;   // 7：帧长 = 7 + LEN

// LEN 的合法范围。★ **这里是契约的一处自相矛盾，实现按"§3 的消息必须能收"取 4**：
//   · §2 写「LEN 载荷字节数；v1 合法范围 **5..16**」；
//   · §3 的 `EVENT 0x40` 载荷是 **4 B**（evt_id u8 + value u16 + face u8）⇒ LEN = 4。
//   照 §2 的字面实现，EVENT 帧会被每一块 v1 板当 bad_len 丢掉，§3 的 EVENT 整个
//   不可用（连带着"用状态帧兜事件"那条设计也失去意义）。所以下限取 **4**
//   （= v1 最短载荷），上限仍是 §2 的 16；契约文字要改的话就是 §2 那一行。
//   ★ 这条已写进回报（那是**契约缺陷**，不是实现选择）。别顺手把 EVENT 加长到
//   5 B 去迁就 §2 —— 那会改掉 §3 的载荷表。
static const uint8_t kLenMin = 4u;
static const uint8_t kLenMax = 16u;
// ★ >64 一律按坏帧丢、**不等载荷**（§2 的 LEN 行）。缓冲区因此不需要
//   按 255 开：最大候选帧 = 7 + 64 = 71 B，§2 建议的 128 B 解析缓冲绰绰有余。
static const uint8_t kLenNoWaitAbove = 64u;
static const uint8_t kFrameBytesMax  = kOverhead + kLenNoWaitAbove;   // 71
static const uint8_t kParseBufBytes  = 128u;                          // §2 的建议值

// ROLE（§2 第 4 字节 / §5）：1 = 主板(右)，0 = 从板(左)
static const uint8_t kRoleSlave  = 0u;
static const uint8_t kRoleMaster = 1u;

// TYPE（§3 的消息表）
enum class MsgType : uint8_t {
  Hello  = 0x01u,   // 双向：版本协商 + 角色对账 + "是不是同一份固件"
  Tick   = 0x10u,   // A→B：时基 + 心跳（车睡着、VAN 没帧时也照发）
  Data   = 0x20u,   // A→B：驱动左盘的四个标量
  Status = 0x30u,   // B→A：单一日志出口的基础 + 链路质量
  Event  = 0x40u,   // 双向（v1 实际只用 B→A）：告警与档位变化，不重传
  // 0x50（从板文本日志转发）与 0x06 事件号是 v2 候选 / 保留段（§3 / §6），
  // 本层**不认**它们：收到按未知 TYPE 丢帧计数。
};

inline bool typeKnown(uint8_t type) {
  switch (type) {
    case (uint8_t)MsgType::Hello:
    case (uint8_t)MsgType::Tick:
    case (uint8_t)MsgType::Data:
    case (uint8_t)MsgType::Status:
    case (uint8_t)MsgType::Event:
      return true;
    default:
      return false;
  }
}
const char* msgTypeName(uint8_t type);   // 日志用；未知返回 "unknown"

// 一帧解出来的结果（只在 decodeFrame 返回 Ok 时被填）
struct Frame {
  uint8_t ver   = 0;        // 线上原值（高 4 位主版本、低 4 位次版本）
  uint8_t type  = 0;        // 已知 TYPE（未知的在 decodeFrame 里就被丢了）
  uint8_t len   = 0;        // 载荷字节数（kLenMin..kLenMax）
  uint8_t role  = 0;        // 发这一帧的那块板的角色（§5 的角色冲突自检用它）
  uint8_t payload[kLenMax] = {0};
  uint16_t crc       = 0;   // 线上字段（15 位值，bit15 恒 0）
  uint16_t crc_calc  = 0;   // 本地算出来的 15 位值（Ok 时两者相等）
  // ★ 主版本不同（§2）：只上报 + 告警，**不丢帧**（已知 TYPE 照 v1 解析）。
  //   次版本不同**不告警**（次版本只用来"加东西"）。
  bool ver_mismatch = false;
};

// 解码结果。Ok 之外的每一条都是 §2「重同步」里"丢这一帧 + 计数"的那几种。
enum class DecodeErr : uint8_t {
  Ok = 0,
  BadSync,        // buf[0] != 0x5A（SYNC 不进 CRC ⇒ 这条必须单独判）
  ShortFrame,     // n < 7：连最短的帧都装不下
  LenOutOfRange,  // LEN 不在 kLenMin..kLenMax（v1 = 4..16，★ 见下）
  LenMismatch,    // n != 7 + LEN：调用方给的字节数与 LEN 不符（半截帧/多给了字节）
  CrcError,       // CRC 字段 != 本地算的 15 位值
  UnknownType,    // TYPE 不在 §3 的表里
};
const char* decodeErrName(DecodeErr e);   // ASCII，日志/用例消息用

// LEN → 整帧字节数（纯算术，不做范围检查：越界值由 lenInRange 判）
inline uint16_t frameBytesForLen(uint8_t len) { return (uint16_t)(kOverhead + len); }
inline bool lenInRange(uint8_t len) { return len >= kLenMin && len <= kLenMax; }

// CRC（§2）：覆盖字节 1..(4+LEN)，即 VER..载荷末尾，长度 n-3。
// frame 必须是**整帧**（n = 7 + LEN）；n < 7 时返回 0（调用方本来就该先判长度）。
uint16_t frameCrc(const uint8_t* frame, uint16_t n);

// 编码。返回写入字节数（= 7 + len）；**返回 0 = 参数非法**，没写任何字节：
//   · out == nullptr / payload == nullptr / cap < 7 + len
//   · len 不在 kLenMin..kLenMax（本层只发 v1 合法长度；>64 那种只可能出现在接收侧）
// ver 参数默认 kVer，只为测试"版本不匹配"造帧用（正常调用别传）。
// ★ 本层**不判 TYPE**：编码是工具/测试/将来 v2 用的，策略判断在接收侧
//   （decodeFrame 拒未知 TYPE）—— 这样"造一个未知 TYPE 的帧"才写得出用例。
uint16_t encodeFrame(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t role,
                     uint8_t* out, uint16_t cap, uint8_t ver = kVer);

// 解码一个**完整**帧（n = 7 + LEN）。判据顺序（每一条都对应 §2 的一条规矩）：
//   ① n < 7                     → ShortFrame
//   ② buf[0] != SYNC            → BadSync
//   ③ LEN > 64                  → LenOutOfRange（**不等载荷**：连 CRC 都不算，
//                                 因为我们根本没有那个缓冲区）
//   ④ n != 7 + LEN              → LenMismatch
//   ⑤ CRC 字段 != 本地算的       → CrcError
//   ⑥ LEN 不在 kLenMin..kLenMax  → LenOutOfRange
//   ⑦ TYPE 未知                 → UnknownType
//   ⑧ 主版本不同                → **Ok** + ver_mismatch=1（§2：不断链、不降级）
// ★ ③ 与 ⑥ 分开写是有意的：LEN 越界但 ≤64 时我们**敢**把整帧收完（缓冲够），
//   于是"CRC 通过但 LEN 越界"= 对端发了一个 v1 认不了的长载荷，计数成 bad_len
//   而不是 crc_err；只有 > 64 才连 CRC 都不算。两种都丢帧并计数（§2），
//   区别只在日志里能不能分清"误码"和"对端跑得比本机新"。
// ★ out == nullptr 或 buf == nullptr 按 ShortFrame 处理（没有可读的字节）。
DecodeErr decodeFrame(const uint8_t* buf, uint16_t n, Frame* out);

}  // namespace dashlink

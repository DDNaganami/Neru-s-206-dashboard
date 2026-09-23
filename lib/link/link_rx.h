#pragma once
#include <stdint.h>

#include "link_frame.h"
#include "link_phy.h"
#include "link_role.h"

// ============================================================
// 双板链路协议 v1 —— **接收侧**（§2 的「重同步」+ §5 的角色冲突）
//
// 把一串字节变成帧，并且**永远不因为一帧坏了就锁死**：
//   · 猎手阶段：丢开所有非 `SYNC` 的字节并计数（回放行、噪声都从这里走，
//     §2 的分流就是靠"首字节 0x56('V') 与 0x5A 不撞"）；
//   · 攒够一帧就交给 decodeFrame()（帧层的五条拒绝路径）；
//   · 不管过不过，**从 SYNC 之后一个字节继续找下一个 SYNC**（§2 原文）——
//     这一条很重要：LEN 字节被改坏时，报错的那段字节里可能就藏着下一帧的
//     真正帧头，整段跳过会白丢一帧。实现方式是把已缓冲的字节**只吐掉一个**
//     再重新找，所以永远回到最后一个候选 SYNC。
//   · 计数口径与 §3 的 STATUS 字段对得上（frames_ok / frames_dropped / crc_err）
//     —— 填 STATUS 的代码直接用 stats() 就行。
//   · §5 ①：收到**对端 ROLE == 本机 ROLE** 的帧 ⇒ 计数 + 置标志 + **丢弃该帧**
//     （定案 L12；不升级为"只收不发"）。本层不会去改本机角色（编译期是唯一权威）。
//
// ★ 两处刻意的设计（都写进注释，免得后面有人"顺手改掉"）：
//   ① **字节先进缓冲、再从缓冲里推进**。所以 feed()/poll() 早退（比如刚解出
//      一帧就返回）**不会丢字节**：剩下的字节还在缓冲里，下一圈接着解。
//   ② 缓冲 128 B（§2 的建议值）已经远超最坏候选帧（LEN ≤ 64 ⇒ 71 B），
//      所以"缓冲满"是一条不可达的防御分支。
// ============================================================

namespace dashlink {

// 计数：与 §3 的 STATUS 载荷字段一一对应（全 u32 累计，写 STATUS 时再截到 u16）
struct LinkRxStats {
  uint32_t frames_ok      = 0;   // 收下并交给上层的帧
  uint32_t crc_err        = 0;   // §2：CRC 不过
  uint32_t bad_len        = 0;   // §2：LEN 越界（含 >64 的"不等载荷"那条）
  uint32_t bad_sync       = 0;   // 候选帧头不对（流里罕见：猎手已经挡掉大部分）
  uint32_t len_mismatch   = 0;   // LEN 与实到字节数不符（半截/多给）
  uint32_t unknown_type   = 0;   // §2：未知 TYPE 一律丢
  uint32_t role_conflict  = 0;   // §5 ①：对端角色与本机相同
  uint32_t noise_bytes    = 0;   // 找 SYNC 时丢掉的非 SYNC 字节（回放行/噪声）

  // "丢掉了几帧" —— STATUS.frames_dropped 用它（§3）
  uint32_t framesDropped() const {
    return crc_err + bad_len + bad_sync + len_mismatch + unknown_type + role_conflict;
  }
};

class LinkRx {
 public:
  // 一次推进的结果
  enum class Step : uint8_t {
    NeedMore = 0,   // 字节还不够，等下一批
    Frame,          // 解出一帧有效帧（已写进 out）
    Reject,         // 丢掉了一帧（err 说明为什么；计数已经加了）
    RoleDrop,       // 帧本身是好的，但因为**角色冲突**被丢（§5 ①；计数 + 标志）
  };

  void reset();

  // 本机角色（编译期宏 LINK_ROLE 的默认值，见 link_role.h）。
  // 只用来做 §5 ① 的自检 —— 本层任何情况下都不会改它。
  void setLocalRole(uint8_t role) { mLocalRole = role; }
  uint8_t localRole() const { return mLocalRole; }

  // 喂**一个**字节（测试/回放用）。
  Step feed(uint8_t b, Frame* out, DecodeErr* err = nullptr);

  // 只推进缓冲里已有的字节（不读 PHY）。feed() = push + advance。
  Step advance(Frame* out, DecodeErr* err = nullptr);

  // 主循环里调：从 PHY **非阻塞**地读字节，直到解出**一帧**或达到 max_bytes；
  // 返回 true ⇒ out 是一帧有效帧。用法：`while (rx.poll(phy, &f)) { ... }`。
  // ★ 一次调用最多读 max_bytes 个字节（默认 64 B ≈ 0.56 ms 线时 @115200），
  //   单次很短、能随时被打断（§1.3）。
  bool poll(LinkPhy& phy, Frame* out, uint16_t max_bytes = 64u);

  bool pending() const { return mLen > 0u; }          // 手上还有没解完的字节
  uint16_t pendingBytes() const { return mLen; }
  const LinkRxStats& stats() const { return mStats; }
  bool verMismatchSeen() const { return mVerMismatch; }     // §2：主版本不匹配告警位
  bool roleConflictSeen() const { return mRoleConflict; }   // §5 ①：STATUS.flags 用

 private:
  void push(uint8_t b);
  void consume(uint16_t k);
  void countDrop(DecodeErr e);

  uint8_t  mBuf[kParseBufBytes] = {0};
  uint16_t mLen  = 0;      // 缓冲里还没消费的字节数（mBuf[0] 开始）
  uint16_t mNeed = 0;      // 当前候选帧的整帧字节数（0 = 还没拿到 LEN）
  bool     mInCandidate = false;   // true ⇒ mBuf[0] 已经是 SYNC，正在攒这一帧
  uint8_t  mLocalRole = kLocalRole;

  LinkRxStats mStats;
  bool mVerMismatch  = false;
  bool mRoleConflict = false;
};

}  // namespace dashlink

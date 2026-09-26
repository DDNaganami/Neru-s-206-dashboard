// 双板链路协议 v1 —— 应用层接线（§1.2 ③ / §3 / §5）。纯逻辑：不碰 UART / LVGL / millis()。
#include "link_app.h"

// ★ 只为 `kLinkBaud`（§1.1 的 115200，唯一出处是 `link_phy_pins.h`）——
//   `lineMsPerSecondForFrame()` 要拿它把"字节数"折成"线时"。
//   那个头是**纯常量 + 编译期守卫**（只 include <stdint.h>，无 Arduino 依赖），
//   所以这一层引它不破坏"lib/link 在宿主机上可编"这条纪律。
#include "link_phy_pins.h"

namespace dashlink {

// ------------------------------------------------------------
// ① 来源编码互转（§3 的 2 位 flags ↔ data_service 的 FieldSource）
// ------------------------------------------------------------
Src fieldSourceToSrc(FieldSource f) {
  switch (f) {
    case FieldSource::None: return Src::None;
    case FieldSource::Sim:  return Src::Sim;
    case FieldSource::Obd:  return Src::Obd;
    case FieldSource::Van:  return Src::Van;
    case FieldSource::Link:
      // ★ Link 编不进 2 位（§3 的 flags 只有 0..3）。取 Sim 而不是 Van 是有意的：
      //   从板对主板报的是"我这四个值从哪来"，而 Link 意味着"主板给什么就是什么"——
      //   那个信息在**主板的** flags 里已经逐字段带过来了。编成 Van 会谎报"我本地
      //   听到了 VAN 总线"（从板连收发器都没有，§0）。
      return Src::Sim;
  }
  return Src::None;
}

FieldSource srcToFieldSource(Src s) {
  switch (s) {
    case Src::None: return FieldSource::None;
    case Src::Sim:  return FieldSource::Sim;
    case Src::Obd:  return FieldSource::Obd;
    case Src::Van:  return FieldSource::Van;
  }
  return FieldSource::None;
}

// ------------------------------------------------------------
// ② DATA 打包/解包（§3 的载荷表 + link_msg.h 的量纲）
// ------------------------------------------------------------
LinkData unpackDataToLinkData(const DataMsg& m, uint32_t rx_ms) {
  LinkData d;
  d.speed_kmh   = rawToSpeedKmh(m.speed_raw);
  d.rpm         = rawToRpm(m.rpm_raw);
  d.coolant_c   = rawToTempC(m.coolant_raw);
  d.intake_c    = rawToTempC(m.intake_raw);
  d.speed_src   = srcToFieldSource(dataFlagsGet(m.flags, kFieldSpeed));
  d.rpm_src     = srcToFieldSource(dataFlagsGet(m.flags, kFieldRpm));
  d.coolant_src = srcToFieldSource(dataFlagsGet(m.flags, kFieldCoolant));
  d.intake_src  = srcToFieldSource(dataFlagsGet(m.flags, kFieldIntake));
  d.rx_ms       = rx_ms;
  return d;
}

DataMsg packLinkData(const VehicleState& st, const DataSourceStatus& src) {
  DataMsg m;
  m.rpm_raw     = rpmToRaw(st.rpm);
  m.speed_raw   = speedToRaw(st.speed_kmh);
  m.coolant_raw = tempToRaw(st.coolant_c);
  m.intake_raw  = tempToRaw(st.intake_c);
  // ★ 唯一入口 dataFlagsPack()，别手写移位（§3 的位号：rpm=bit7..6、speed=bit5..4、
  //   coolant=bit3..2、intake=bit1..0）。
  m.flags = dataFlagsPack(fieldSourceToSrc(src.rpm), fieldSourceToSrc(src.speed),
                          fieldSourceToSrc(src.coolant), fieldSourceToSrc(src.intake));
  return m;
}

// ------------------------------------------------------------
// ②' VAN 原始帧 ↔ VanPacket（`0x21 VANRAW`）
// ------------------------------------------------------------
VanRawMsg vanRawFromPacket(const VanPacket& pkt) {
  VanRawMsg m;
  m.iden = (uint16_t)(pkt.iden & 0x0FFFu);   // 12 位有效（VanPacket 的口径）
  m.cmd = (uint8_t)(pkt.cmd & 0x0Fu);        // 4 位（同上）
  m.ack = pkt.ack != 0u;
  m.fcs_ok = pkt.fcs_ok != 0u;
  // ★ 超长**不在这里截断**：截断的帧在从板会被当成"另一个帧"解出错误的字段值。
  //   这里照实把 len 填成原值（可能 > kVanRawMaxData），由 `VanRawQueue::push()`
  //   判"放不下 ⇒ 整帧丢掉 + 计数"。`data` 只拷得下多少拷多少 —— 反正
  //   `push()` 不会把它发出去（`packVanRaw()` 见到 len 越界就返回 0）。
  m.len = pkt.len;
  const uint8_t n = (pkt.len > kVanRawMaxData) ? kVanRawMaxData : pkt.len;
  for (uint8_t i = 0; i < n; ++i) m.data[i] = pkt.data[i];
  return m;
}

VanPacket unpackVanRawToVanPacket(const VanRawMsg& m, uint32_t rx_ms) {
  VanPacket p{};
  p.iden = (uint16_t)(m.iden & 0x0FFFu);
  p.cmd = (uint8_t)(m.cmd & 0x0Fu);
  p.ack = m.ack ? 1u : 0u;
  // ★ `fcs_ok` **原样搬**，不在从板重算：链路的 CRC 保证的是"这 16 字节没被改过"，
  //   而 VAN 帧自己的 FCS 是**主板**在线上算的 —— 从板既没有那串位流、也不该
  //   假装自己验过。丢掉这一位会让从板把主板已经判坏的帧当好的用。
  p.fcs_ok = m.fcs_ok ? 1u : 0u;
  p.len = m.len;
  for (uint8_t i = 0; i < m.len; ++i) p.data[i] = m.data[i];
  p.rx_ms = rx_ms;
  return p;
}

// ------------------------------------------------------------
// ②'' 原始帧转发队列
// ------------------------------------------------------------
void VanRawQueue::reset() {
  mTail = 0;
  mCount = 0;
  mPushed = 0;
  mDropped = 0;
  mTooLong = 0;
}

bool VanRawQueue::push(const VanRawMsg& m) {
  if (m.len > kVanRawMaxData) {   // 装不下（例如 VIN 的 17 字节）⇒ 丢这一帧
    ++mTooLong;
    return false;
  }
  uint8_t rec[kLenMax];
  const uint8_t n = packVanRaw(m, rec);
  if (n == 0u) {                  // 参数非法（理论上到不了这里，兜一层）
    ++mTooLong;
    return false;
  }
  if ((uint16_t)(mCount + n) > kRingBytes) {   // 放不下整帧 ⇒ 丢这一帧（绝不写半帧）
    ++mDropped;
    return false;
  }
  uint16_t w = (uint16_t)((mTail + mCount) % kRingBytes);
  for (uint8_t i = 0; i < n; ++i) {
    mBuf[w] = rec[i];
    w = (uint16_t)((w + 1u) % kRingBytes);
  }
  mCount = (uint16_t)(mCount + n);
  ++mPushed;
  return true;
}

uint8_t VanRawQueue::pop(uint8_t* out, uint8_t cap) {
  if (out == nullptr || cap < kLenMax) return 0u;   // 容量不够：不动环
  if (mCount < kVanRawHdrLen) return 0u;            // 空（或残了一段不可能存在的尾巴）
  // 记录长度写在**载荷第 3 个字节**（dlen）里 ⇒ 先把它读出来（可能跨环尾）。
  const uint8_t dlen = mBuf[(uint16_t)((mTail + 2u) % kRingBytes)];
  if (dlen > kVanRawMaxData) return 0u;             // 环里不该出现这种记录
  const uint8_t n = (uint8_t)(kVanRawHdrLen + dlen);
  if (mCount < n) return 0u;
  uint16_t r = mTail;
  for (uint8_t i = 0; i < n; ++i) {
    out[i] = mBuf[r];
    r = (uint16_t)((r + 1u) % kRingBytes);
  }
  mTail = r;
  mCount = (uint16_t)(mCount - n);
  return n;
}

// ------------------------------------------------------------
// ③ DATA 的发送节奏
// ------------------------------------------------------------
void DataSender::reset() {
  mHasSent = false;
  mLastUpdateMs = 0;
  mLastSentMs = 0;
  mSent = 0;
}

bool DataSender::due(uint32_t now_ms, uint32_t snapshot_ms, const VehicleState& st,
                     const DataSourceStatus& src, DataMsg* out) {
  if (out == nullptr) return false;

  // ① 本轮的"快照时刻"。
  //    snapshot_ms == 0 有两层含义，两层都走"照发"：
  //      · 本机到现在还没收到过任何一帧 0x824（VanSource::last_update_ms 初值就是 0）；
  //      · 调用方明确表示"这一路没有 VAN 快照可跟"。
  //    为什么那时**还要**发：车睡着、VAN 没帧的时候从板照样得动起来（§3 把 DATA 的
  //    超时策略与 TICK 归成一档："车睡着、VAN 没帧时也照发"）。从板上没有本地源 ⇒
  //    不发它就只剩 Sim 假数据了。速率由调用的主循环周期 + ③ 的限速压住；就算真发快，
  //    `LinkTx` 的环满会整帧丢（§1.2 ②）—— 不会打爆链路。
  //    ★ 这一路把 now_ms 当快照时刻用（"每次调用都是新的一份快照"）。
  const uint32_t stamp = (snapshot_ms != 0u) ? snapshot_ms : now_ms;

  // ② 没有**新的**快照 ⇒ 不发（§3：DATA 跟随 0x824 到达，**不另建定时器**）。
  //    这一条只在**有 VAN** 时才有意义：没有 VAN 时 stamp 恒等于 now_ms（每次调用都
  //    是一个新时刻，那正是上面的"照发"），由 ③ 的限速兜着。
  if (snapshot_ms != 0u && mHasSent && snapshot_ms == mLastUpdateMs) return false;

  // ③ 可选下限（默认 0 = 不限）。判据是"距**上一份发出去的**快照过了多久"：
  //    · 窗口内的快照被**丢掉**（不是排队补发，也不是延长窗口）；
  //    · 窗口一过，发的是**当下最新**的那一份 —— §1.2 ③ 要的就是这个；
  //    · mLastUpdateMs **只在真的发出去时才推进**（就在下面那一行），
  //      所以被丢掉的快照不会把窗口越推越远（否则 min=100 会变成"每来一份快照就
  //      再锁 100 ms"，节流就成了自锁）。
  // ★ 为什么这也顺带实现了"不补发突发"：一圈里 fast-forward 了好几个 0x824 时，
  //   只有第一份能满足"距上一份发出的快照 ≥ min"，其余的都在窗口内被丢掉。
  if (mHasSent && mMinIntervalMs != 0u &&
      (int32_t)(stamp - mLastUpdateMs) < (int32_t)mMinIntervalMs) {
    return false;
  }
  mLastUpdateMs = stamp;
  mHasSent = true;

  *out = packLinkData(st, src);
  mLastSentMs = now_ms;
  ++mSent;
  return true;
}

// ------------------------------------------------------------
// ③ STATUS 的发送节奏（§3 的 `0x30` 行：2 Hz / 500 ms）
// ------------------------------------------------------------
void StatusSender::reset() {
  mHasSent    = false;
  mLastSentMs = 0;
  mSent       = 0;
}

bool StatusSender::due(uint32_t now_ms, StatusMsg* out) {
  if (out == nullptr) return false;
  // ★ 有符号差 ⇒ `millis()` 回绕天然正确（与 §4 的 d、TickGen::due 同一个套路）。
  //   `!mHasSent` 那一支保证**上电第一次立刻发**（否则要等满 500 ms 才有第一行
  //   `link: B uptime=…`，而"从板到底活没活"正要看那第一行 —— 本单之前那一行
  //   **永远打不出来**，因为从板一条 STATUS 都不发）。
  if (mHasSent && (int32_t)(now_ms - mLastSentMs) < (int32_t)kStatusPeriodMs) return false;

  // ★ 只填**属于本层**的两个字段，其余一律留给调用方（它才认识固件版本、接收计数、
  //   屏上档位）：
  //     · `uptime_ms` —— 契约就是"从复位起算的单调毫秒"，而 `now_ms` 正是调用方
  //       传进来的那个 millis() ⇒ 这里填它，调用方不必再传一遍；
  //     · `build_tag` —— §3 的 HELLO 用它，STATUS 表里没有这一项，**不填**。
  // ★ 其余字段**一个都不碰**（包括 `last_gap_ms`）：§3 定案"字段保留、v1 一律发 0、
  //   没有生产者" ⇒ 让调用方的 `StatusMsg{}` 默认值自然流过去。在这里补一行 `= 0`
  //   会把"没有生产者"伪装成"生产者说是 0"（两者将来要分开看）。
  out->uptime_ms = now_ms;

  mLastSentMs = now_ms;
  mHasSent    = true;
  ++mSent;
  return true;
}

uint8_t slaveStatusFlags(bool ver_mismatch, bool role_conflict, LinkTimeState data_state) {
  uint8_t f = 0u;
  if (ver_mismatch)  f = (uint8_t)(f | kStFlagVerMismatch);
  if (role_conflict) f = (uint8_t)(f | kStFlagRoleConflict);
  // §3 的 DATA 行：跟 TICK 同一条三级超时 ⇒ "不是 Locked"就是"没有新鲜的 DATA"。
  if (data_state != LinkTimeState::Locked) f = (uint8_t)(f | kStFlagNoData);
  // ★ `kStFlagTempNoSource` 恒不置位 —— 生产者不存在，理由见 link_app.h 那一段。
  return f;
}

// ------------------------------------------------------------
// ④ 发送预算（§1.1 那张占空比表的算术化）
// ------------------------------------------------------------
uint32_t lineMsPerSecondForFrame(uint8_t payload_len, uint32_t frames_per_second) {
  if (frames_per_second == 0u) return 0u;
  // 一帧的 bit 数 = 整帧字节 × 10（8N1：1 起始 + 8 数据 + 1 停止）
  // 线时(ms/s) = 帧率 × bit数 × 1000 / kLinkBaud
  const uint32_t bytes = (uint32_t)frameBytesForLen(payload_len);
  const uint32_t bits  = bytes * 10u;
  const uint32_t num   = frames_per_second * bits * 1000u;
  // ★ **四舍五入到整毫秒**，不是截断。为什么这一条要专门写：
  //   契约 §1.1 的原话是 "`STATUS`（23 B）**2.00 ms**"、"`TICK`（12 B）1.04 ms" ——
  //   23 B @115200 8N1 的真值是 **1.9965 ms**。整除截断会把它算成 **1**，
  //   于是"照契约算出来的预算"与作者写下的那个数字**差一倍**，
  //   而这条函数的全部意义就是与契约那张表对账（用例逐条钉着）。
  //   ⇒ 取最接近的整数：1.9965 → 2；1.0417 → 1（两者都与 §1.1 写的一致）。
  //   ★ 分子最大量级：50 帧/s × 230 bit × 1000 = 1.15e7 ⇒ u32 绰绰有余（不溢出）。
  return (num + (kLinkBaud / 2u)) / kLinkBaud;
}

uint32_t linkBudgetMsPerSecond(uint32_t master_data_hz, uint32_t master_tick_hz,
                               uint32_t slave_status_hz, uint32_t slave_hello_hz,
                               uint32_t master_vanraw_hz) {
  // ★ VANRAW 这一项（2026-09-27 新增）用的是**车速帧那种长度**的载荷
  //   （`kVanRawHdrLen + 7` = 11 B，与 `kDataLen` 一样），因为总线上
  //   ≈80 Hz 的那一帧就是它；最长的可搬帧（灯位，15 B 载荷）只有 4.7 Hz，
  //   对预算的贡献可以忽略。**别把这一项当成"最坏情况"** ——
  //   真正的上界是 `kVanRawHdrLen + kVanRawMaxData` = 16 B，若要按上界算，
  //   在这里显式改用那个常量（用例会跟着变）。
  return lineMsPerSecondForFrame(kDataLen, master_data_hz) +
         lineMsPerSecondForFrame((uint8_t)(kVanRawHdrLen + 7u), master_vanraw_hz) +
         lineMsPerSecondForFrame(kTickLen, master_tick_hz) +
         lineMsPerSecondForFrame(kStatusLen, slave_status_hz) +
         lineMsPerSecondForFrame(kHelloLen, slave_hello_hz);
}

bool handleInbound(const Frame& f, LinkTime* t, uint32_t now_ms, LinkData* out_data) {
  switch (f.type) {
    case (uint8_t)MsgType::Tick: {
      TickMsg m;
      if (!unpackTick(f.payload, f.len, &m)) return false;
      if (t != nullptr) t->onTick(m, now_ms);
      return true;
    }
    case (uint8_t)MsgType::Data: {
      DataMsg m;
      if (!unpackData(f.payload, f.len, &m)) return false;
      if (t != nullptr) t->onData(now_ms);
      if (out_data != nullptr) *out_data = unpackDataToLinkData(m, now_ms);
      return true;
    }
    default:
      return false;   // HELLO/STATUS/EVENT：交给调用方（日志/角色对账）
  }
}

}  // namespace dashlink

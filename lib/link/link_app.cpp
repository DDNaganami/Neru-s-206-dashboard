// 双板链路协议 v1 —— 应用层接线（§1.2 ③ / §3 / §5）。纯逻辑：不碰 UART / LVGL / millis()。
#include "link_app.h"

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

#include "data_service.h"
#include "sim_source.h"

static const uint32_t kStaleMs = 3000;  // 源超时回退阈值

const char* fieldSourceName(FieldSource f) {
  switch (f) {
    case FieldSource::None: return "none";
    case FieldSource::Sim:  return "sim";
    case FieldSource::Obd:  return "obd";
    case FieldSource::Van:  return "van";
    case FieldSource::Link: return "link";
  }
  return "?";
}

static bool fresh(uint32_t last_ms, uint32_t now_ms) {
  return last_ms != 0 && (now_ms - last_ms) < kStaleMs;
}

void VehicleDataService::begin() {
  obd_.begin();
  van_.begin();
}

VehicleState VehicleDataService::update(uint32_t now_ms) {
  // 1) 假数据打底:无任何硬件时 UI 也能动
  sim_update(state_, now_ms);
  status_.speed = FieldSource::Sim;
  status_.rpm = FieldSource::Sim;
  status_.coolant = FieldSource::Sim;
  status_.intake = FieldSource::Sim;
  status_.fuel = FieldSource::Sim;
  status_.gear = FieldSource::Sim;
  status_.speed_age_ms = UINT32_MAX;
  status_.rpm_age_ms = UINT32_MAX;
  status_.coolant_age_ms = UINT32_MAX;
  status_.intake_age_ms = UINT32_MAX;

  // 2) K 线 OBD:转速 / 水温 / 进气温度(高优先级,按字段独立超时:
  //    转速断了不影响水温继续用 OBD,反之亦然)
  obd_.tick(now_ms);
  if (obd_.enabled() && obd_.hasRpm() && fresh(obd_.lastRpmMs(), now_ms)) {
    state_.rpm = obd_.rpm();
    status_.rpm = FieldSource::Obd;
    status_.rpm_age_ms = now_ms - obd_.lastRpmMs();
  }
  if (obd_.enabled() && obd_.hasCoolant() && fresh(obd_.lastCoolantMs(), now_ms)) {
    state_.coolant_c = obd_.coolant();
    status_.coolant = FieldSource::Obd;
    status_.coolant_age_ms = now_ms - obd_.lastCoolantMs();
  }
  // ★ 进气温度只有 OBD 这一个真源(206 没有 VAN 上的进气温度,VAN 落点也没这项);
  //   拿不到就留在假数据上,和车速缺 VAN 时的处理一致。
  //   它**不参与表情**(和冷却液一样只驱动弧 + 数字),所以回退不会造成"脸乱变"。
  if (obd_.enabled() && obd_.hasIntake() && fresh(obd_.lastIntakeMs(), now_ms)) {
    state_.intake_c = obd_.intake();
    status_.intake = FieldSource::Obd;
    status_.intake_age_ms = now_ms - obd_.lastIntakeMs();
  }
  // ★ 车速的 OBD 兜底(010D):只在 ECU 支持、而且我们确实在问它的时候才有值。
  //   这里**不判断**"VAN 有没有给"—— 下面的 VAN 段在后面,它会直接覆盖,
  //   天然实现"Van > Obd"。顺序就是优先级,别把两段调过来。
  if (obd_.enabled() && obd_.hasSpeed() && fresh(obd_.lastSpeedMs(), now_ms)) {
    state_.speed_kmh = obd_.speed();
    status_.speed = FieldSource::Obd;
    status_.speed_age_ms = now_ms - obd_.lastSpeedMs();
  }
  // OBD 的实测刷新率与 0100 位图的结论(只读,不参与合并)
  status_.obd_rpm_hz = obd_.rpmHz();
  status_.obd_coolant_hz = obd_.coolantHz();
  status_.obd_intake_hz = obd_.intakeHz();
  status_.obd_speed_hz = obd_.speedHz();
  status_.obd_support_known = obd_.speedSupportKnown();
  status_.obd_support_mask = obd_.supportMask();
  status_.obd_speed_polled = obd_.speedPolled();
  status_.obd_speed_supported =
      obd_.speedSupportKnown() ? (obd_.speedSupported() ? 1 : 0) : -1;

  // 3) VAN:车速(最高优先级);转速作为 OBD 缺失时的补充
  van_.tick(now_ms);
  if (van_.hasSpeed() && fresh(van_.lastUpdateMs(), now_ms)) {
    state_.speed_kmh = van_.speedKmh();
    status_.speed = FieldSource::Van;
    status_.speed_age_ms = now_ms - van_.lastUpdateMs();
  }
  if (van_.hasRpm() && fresh(van_.lastUpdateMs(), now_ms) &&
      status_.rpm == FieldSource::Sim) {
    state_.rpm = van_.rpm();
    status_.rpm = FieldSource::Van;
    status_.rpm_age_ms = now_ms - van_.lastUpdateMs();
  }

  // 4) 链路（从板侧，2026-09-23 新增）—— ★ 只在"这一格本来要落到 Sim"时接手。
  //
  //    为什么这样写（这是本轮唯一改 data_service 的地方，口径必须写清）：
  //      · 上面 1)~3) 三段**一个字都没动** ⇒ 既有优先级
  //        （车速 Van > Obd > Sim、转速 Od > Van > Sim、水温/进气 Obd > Sim）
  //        与它们的**边界行为**（3 秒回退、按字段独立）全部原样保留。
  //      · 链路这一段放在**最后**、且以 `status_.x == FieldSource::Sim` 为前置条件
  //        ⇒ 它只可能**填掉 Sim 的空位**，不可能抢走 Van/Obd 已经拿下的字段。
  //        于是"链路把真值压掉"这类事故在结构上不可能发生（有两条用例钉住）。
  //      · 从板本机没有任何本地源（不接 VAN 硬件、通常也没有 OBD）⇒ 在从板上
  //        "该字段仍是 Sim"就等于"本地什么都没有"；而链路上过来的 DATA 带着
  //        **每字段的来源位**（§3 的 flags），所以"这个值是真值还是假数据"跟着数据
  //        一起到了 —— 本段只用"新不新鲜"这一条判据（与其它源同一套 3 秒规则）。
  if (link_seen_) {
    const bool link_fresh = fresh(link_.rx_ms, now_ms);
    const uint32_t age = link_fresh ? (now_ms - link_.rx_ms) : UINT32_MAX;
    if (link_fresh && link_.speed_src != FieldSource::None &&
        status_.speed == FieldSource::Sim) {
      state_.speed_kmh = link_.speed_kmh;
      status_.speed = FieldSource::Link;
      status_.speed_age_ms = age;
    }
    if (link_fresh && link_.rpm_src != FieldSource::None &&
        status_.rpm == FieldSource::Sim) {
      state_.rpm = link_.rpm;
      status_.rpm = FieldSource::Link;
      status_.rpm_age_ms = age;
    }
    if (link_fresh && link_.coolant_src != FieldSource::None &&
        status_.coolant == FieldSource::Sim) {
      state_.coolant_c = link_.coolant_c;
      status_.coolant = FieldSource::Link;
      status_.coolant_age_ms = age;
    }
    if (link_fresh && link_.intake_src != FieldSource::None &&
        status_.intake == FieldSource::Sim) {
      state_.intake_c = link_.intake_c;
      status_.intake = FieldSource::Link;
      status_.intake_age_ms = age;
    }
  }

  return state_;
}

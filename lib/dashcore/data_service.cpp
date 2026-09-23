#include "data_service.h"
#include "sim_source.h"

static const uint32_t kStaleMs = 3000;  // 源超时回退阈值

// 门信号的"活动"窗口(ms)。★ 这个数与 `kStaleMs`(3 秒)**刻意不同**,别合并:
//   · 3 秒是"这个源还活着吗"的判据(所有标量字段共用);
//   · 本窗口是"门这一整套动作还在进行吗"的判据 —— 开门 → 上车/下车 → 关门
//     是一串持续几秒的脉冲,所以取得比 3 秒长。
//   ★ 取 8 秒是**工程判断,不是实测数字**:抓包那份数据里门脉冲是 10 s 档
//     (第四轮实验:左门开/关/开/关 各 10 s),所以 8 s 落在"一次开门动作之内"。
//     真车标定要等 owner 拿这块屏在地库里试一次(见回报里"留待裁决"那节)。
//   ★ 它只影响**告警是否续着报**,不影响数据层的来源标注(那一格只看"收到过没有")。
static const uint32_t kDoorActivityMs = 8000;

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

  // 5) VAN 已解出的四类字段（2026-09-24 新增）—— ★ 本段是**独立的一段**，
  //    不插进 1)~4) 任何一段里，理由三条（这就是"没动既有优先级"的证据）：
  //
  //      ① 这四类字段**没有第二来源**（转向灯/灯位/门/VIN 在 OBD-II 里没有对应
  //         PID，模拟页也不产生它们）⇒ 它们不进 `status_.x == Sim` 那套填空位
  //         逻辑，`applyLinkData` 也一个字没改 ⇒ 结构上不可能抢走 speed/rpm/
  //         coolant/intake 任何一格，也不可能被 Link 覆盖（链路只搬那四个标量）。
  //      ② 判据用的是**自己那颗时间戳**（`van_.lightsLastMs()` / `vinLastMs()`），
  //         与车速/转速共用的 `van_.lastUpdateMs()` 分开 —— 这正是"按字段独立"
  //         那条纪律（test_obd_intake_takeover_per_field 钉过一次的那个坑：
  //         共用时间戳会让一路有数据就把另一路也判成有数据）。
  //         实测这三族的速率差得远：0x824 ≈ 9.7 Hz、0x4FC = 4.7 Hz、0xE24 是常量广播。
  //      ③ 灯位刻意**不吃** `kStaleMs`（3 秒）那套：它的窗口是 600 ms
  //         （`kIndicatorHoldMs`，见 van_source.h 的推导）。3 秒对闪着的转向灯
  //         是"熄了还亮三秒"，对真灭灯也是"三秒才灭" —— 两个方向都错。
  //
  //    灯位的 `age` 只报**最近一帧**的年龄（真实新鲜度），"灯有没有过保持窗口"
  //    由 `indicator_*` 这几个 bool 承担 —— 两者分开，UI 才画得出"数据在、但灯灭"。
  if (van_.hasLights()) {
    const uint32_t lage = now_ms - van_.lightsLastMs();
    status_.lights_age_ms = lage;
    if (van_.lightsRecent(now_ms)) {
      const VanLights& L = van_.lights();
      state_.indicator_left = L.left;
      state_.indicator_right = L.right;
      state_.hazard = L.hazard;
      state_.position_lamp = L.dashboard;
      state_.low_beam = L.low_beam;
      // 五格**同一个来源**（同一帧的同一个字节），所以一起标、不分开判
      status_.indicator_left = FieldSource::Van;
      status_.indicator_right = FieldSource::Van;
      status_.hazard = FieldSource::Van;
      status_.position_lamp = FieldSource::Van;
      status_.low_beam = FieldSource::Van;
    } else {
      // 过窗：值**清掉**（不是停在最后那个状态）—— 闪着的灯不能让屏上留个僵尸箭头。
      // 来源回 `None` = "这一格现在没有有效值"，与 LinkData 里 None 的语义一致。
      state_.indicator_left = false;
      state_.indicator_right = false;
      state_.hazard = false;
      state_.position_lamp = false;
      state_.low_beam = false;
      status_.indicator_left = FieldSource::None;
      status_.indicator_right = FieldSource::None;
      status_.hazard = FieldSource::None;
      status_.position_lamp = FieldSource::None;
      status_.low_beam = FieldSource::None;
    }
  }
  if (van_.hasDoor()) {
    // ★ 门这一格报的是"**动过没有**"，不是"门开着"（左右门不可分辨 = 未解，
    //   §6 撤回①）。窗口取 `kDoorActivityMs`：它要覆盖"开门这一整套动作"
    //   （人下车、关门），比灯位的 600 ms 长得多。
    // ★ age 只在**真的变化过**之后才算得出来（doorChangeMs() == 0 = 从未变化）：
    //   否则会报出 `now - 0` 这种看着像"刚发生"的假年龄。没变化 ⇒ 保持哨兵值。
    const uint32_t dchg = van_.doorChangeMs();
    if (dchg != 0u) status_.door_age_ms = now_ms - dchg;
    state_.door_activity = van_.doorActivity(now_ms, kDoorActivityMs);
    // 来源格是"这一格有没有数据"：收到过门帧就一直是 Van（见 DataSourceStatus ②）。
    status_.door = FieldSource::Van;
  }
  if (van_.hasVin()) {
    status_.vin = FieldSource::Van;
    status_.vin_age_ms = now_ms - van_.vinLastMs();
    if (state_.vin[0] == '\0') {
      // 只在还是空串时拷一次（17 字节 + 结尾）：这是**常量广播**，
      // 每帧都拷只是白费 CPU（0xE24 ≈ 0.9 Hz，拷了也不会变）。
      const char* v = van_.vin();
      for (uint8_t i = 0; i <= kVanVinChars; ++i) state_.vin[i] = v[i];
    }
  }

  return state_;
}

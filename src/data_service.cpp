#include "data_service.h"
#include "sim_source.h"

static const uint32_t kStaleMs = 3000;  // 源超时回退阈值

const char* fieldSourceName(FieldSource f) {
  switch (f) {
    case FieldSource::None: return "none";
    case FieldSource::Sim:  return "sim";
    case FieldSource::Obd:  return "obd";
    case FieldSource::Van:  return "van";
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
  status_.fuel = FieldSource::Sim;
  status_.gear = FieldSource::Sim;
  status_.speed_age_ms = UINT32_MAX;
  status_.rpm_age_ms = UINT32_MAX;
  status_.coolant_age_ms = UINT32_MAX;

  // 2) K 线 OBD:转速 / 水温(高优先级)
  obd_.tick(now_ms);
  if (obd_.enabled() && obd_.hasRpm() && fresh(obd_.lastUpdateMs(), now_ms)) {
    state_.rpm = obd_.rpm();
    status_.rpm = FieldSource::Obd;
    status_.rpm_age_ms = now_ms - obd_.lastUpdateMs();
  }
  if (obd_.enabled() && obd_.hasCoolant() && fresh(obd_.lastUpdateMs(), now_ms)) {
    state_.coolant_c = obd_.coolant();
    status_.coolant = FieldSource::Obd;
    status_.coolant_age_ms = now_ms - obd_.lastUpdateMs();
  }

  // 3) VAN:车速(高优先级);转速作为 OBD 缺失时的补充
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

  return state_;
}

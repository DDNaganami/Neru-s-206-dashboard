#pragma once
#include <Arduino.h>
#include "vehicle_state.h"
#include "obd_source.h"
#include "van_source.h"

enum class FieldSource : uint8_t { None = 0, Sim, Obd, Van };
const char* fieldSourceName(FieldSource f);

struct DataSourceStatus {
  FieldSource speed    = FieldSource::None;
  FieldSource rpm      = FieldSource::None;
  FieldSource coolant  = FieldSource::None;
  FieldSource intake   = FieldSource::None;
  FieldSource fuel     = FieldSource::None;
  FieldSource gear     = FieldSource::None;
  uint32_t speed_age_ms   = UINT32_MAX;
  uint32_t rpm_age_ms     = UINT32_MAX;
  uint32_t coolant_age_ms = UINT32_MAX;
  uint32_t intake_age_ms  = UINT32_MAX;
};

// 多源数据合并服务。
// 优先级:车速 Van > Sim;转速/水温/进气温度 Obd > Van > Sim;油量/挡位 Sim。
// 高优先级源超过 3 秒无新数据自动回退下一源(行车中拔线/OBD 断连不黑屏)。
//
// ★ 进气温度(010F)与水温走同一套规则,但它**没有 VAN 备份源** ——
//   206 的 VAN 上没有这一项。所以 OBD 一断,它就回到假数据值,
//   表现是"弧停在某个位置不动",而不是黑屏或乱跳。
class VehicleDataService {
public:
  // obd_serial: 接 ELM327 的串口(波特率由外部配置);nullptr = 不启用 OBD
  explicit VehicleDataService(HardwareSerial* obd_serial = nullptr)
      : obd_(obd_serial) {}

  void begin();
  VehicleState update(uint32_t now_ms);
  const DataSourceStatus& status() const { return status_; }

  // VAN 物理层(SN65HVD230 + VanBus 库)接好后,驱动把收到的帧喂进来
  void onVanPacket(const VanPacket& pkt) { van_.onPacket(pkt); }
  VanSource& vanSource() { return van_; }

private:
  ObdSource obd_;
  VanSource van_;
  VehicleState state_;
  DataSourceStatus status_;
};

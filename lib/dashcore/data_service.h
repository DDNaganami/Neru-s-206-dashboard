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

  // OBD 各字段的**实测刷新率**(Hz,每秒结算一次)。
  // 为什么放在 status 里:这是"K 线够不够用"的唯一判据,而 K 线是一条
  // 排队共享的窄管子 —— 加一个 PID 会不会把别的字段拖慢,算不出来
  // (ELM327 自身开销 + ECU 响应快慢都在里面),只能实测。见 obd_source.h。
  float obd_rpm_hz     = 0.0f;
  float obd_coolant_hz = 0.0f;
  float obd_intake_hz  = 0.0f;
  float obd_speed_hz   = 0.0f;

  // 车速那条 OBD 通路的三个状态(别混,见 obd_source.h 的同名注释):
  //   -1 = 还没问到 0100 位图(或没接 OBD) / 0 = ECU 说不支持 / 1 = 支持
  int8_t obd_speed_supported = -1;
  bool   obd_speed_polled    = false;   // 我们实际有没有排进轮询表
  bool   obd_support_known   = false;   // 位图到底拿到了没
  uint32_t obd_support_mask  = 0;       // 0100 位图原文(bit31..0 ↔ PID 01..20)
};

// 多源数据合并服务。
// 优先级:车速 Van > Obd > Sim;转速 Obd > Van > Sim;
//         **水温/进气温度只有 Obd > Sim**(这两项 VAN 帧里没有,没有第二来源);
//         油量/挡位 Sim。
// 高优先级源超过 3 秒无新数据自动回退下一源(行车中拔线/OBD 断连不黑屏)。
//
// ★ 车速为什么是 Van 优先(而不是 Obd 优先):
//   K 线是**请求/应答**且排队共享,而车速和转速是最需要"跟手"的两条弧。
//   VAN 上的车速是仪表在总线上**广播**的 —— 纯听,零 K 线成本,
//   于是 K 线那个时隙可以留给转速。OBD 的 010D 是**兜底**:
//   VAN 还没接/还没解出帧时,车速照样有真值(≈1Hz),而不是掉回假数据。
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

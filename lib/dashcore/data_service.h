#pragma once
#include <Arduino.h>
#include "vehicle_state.h"
#include "obd_source.h"
#include "van_source.h"

// ★ 2026-09-23 新增第 5 档 Link（= 板间链路 / 主机）。
//   出处：ARCHITECTURE.md §3 表下那条**实现侧自记的缺口** ——「`data_service.h` 的
//   `FieldSource` 只有 None / Sim / Obd / Van 四档，**没有"主机 / 链路"这一档** ——
//   从板要把收到的 `DATA` 喂进它自己的 `data_service`，得先加一档」。
//
//   ★ 数值口径两条（都别动，动了会静默错）：
//     ① 前三档 None=0 / Sim=1 / Obd=2 / Van=3 的**数值一个都没改** ——
//        §3 的 `DATA.flags` 那 2 位就是以这四档为编码的（`link_msg.h` 的 `Src`
//        与这里刻意对齐，且有静态对账用例），改数值等于改协议。
//     ② **Link 排在最后（= 4）**：它比 Van 晚出现，而 `DATA.flags` 只有 2 位
//        （只能装 0..3）⇒ **Link 不上链路**（从板不需要把链路来源再报回去）。
static const uint8_t kFieldSourceLinkValue = 4u;
enum class FieldSource : uint8_t { None = 0, Sim, Obd, Van, Link };
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

// 板间链路（主板 → 从板）送过来的**一份快照**。
//
// 为什么要单独一个结构、而不是让 data_service 直接吃 link::DataMsg：
//   `data_service.h` 是**数据层**（`-I lib/dashcore`），而 `DATA` 的载荷布局是**协议层**
//   （`-I lib/link`）。让数据层 include 协议头会倒过来依赖，而且 `link_msg.h` 里那套
//   量纲换算函数（rpmToRaw / rawToRpm…）是"线上格式"的事，数据层只需要**已经换算好的
//   物理量**。所以这里只收四个标量 + 每字段的来源 + 收到它的时刻；协议 ↔ 本结构之间
//   那一步换算放在 `lib/link/link_app.h`（`unpackDataToLinkData()`）。
//
// 时间戳的用途与 `VanPacket::rx_ms` 完全一样：`update()` 用它做"3 秒无新数据回退"
// （§3 的 DATA 行：同 TICK 那三档；>3 s ⇒ 回退 Sim，沿用 data_service 既有规则）。
struct LinkData {
  float speed_kmh = 0.0f;
  float rpm = 0.0f;
  float coolant_c = 0.0f;
  float intake_c = 0.0f;
  // 每字段的来源。★ `None` 的含义是"这一格没有有效值" ⇒ 该字段**不参与覆盖**，
  //   留在当前值（通常是 Sim）上。从板收到 `flags` 里某一格为 None 时正是这个语义。
  FieldSource speed_src = FieldSource::None;
  FieldSource rpm_src = FieldSource::None;
  FieldSource coolant_src = FieldSource::None;
  FieldSource intake_src = FieldSource::None;
  uint32_t rx_ms = 0;   // 收到这份快照的本机毫秒（喂给 update() 的 now_ms 同一时基）
};

// 多源数据合并服务。
// 优先级:车速 Van > Obd > Sim;转速 Obd > Van > Sim;
//         **水温/进气温度只有 Obd > Sim**(这两项 VAN 帧里没有,没有第二来源);
//         油量/挡位 Sim。
// 高优先级源超过 3 秒无新数据自动回退下一源(行车中拔线/OBD 断连不黑屏)。
//
// ★ 2026-09-23：上面这套优先级**一个字都没改**，新增的链路（Link）只在"该字段本来
//   就要落到 Sim"时才接手 —— 见 applyLinkData() 的注释与
//   test_data_service.cpp 里那两条"既有优先级不变"的用例。
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

  // 板间链路（从板侧）：把收到的 DATA 解出来的快照喂进来。
  // ★ 调用时机（对应 §1.2 ③ 的口径）：在**主循环**里、`update(now)` **之前**调；
  //   绝不在 UART 的接收回调/ISR 里调这个（那条路径只该往队列里放字节）。
  // 语义见 LinkData 的注释：每字段独立，None 的字段不覆盖。
  void applyLinkData(const LinkData& d) { link_ = d; link_seen_ = true; }

private:
  // §3 §5 之外的一处"数据层"细节：链路快照的新鲜度判据与其它源一致（3 秒，
  // 见 .cpp 的 kStaleMs）。这里只存"最近一次收到的时刻"。
  LinkData link_;
  bool link_seen_ = false;

  ObdSource obd_;
  VanSource van_;
  VehicleState state_;
  DataSourceStatus status_;
};

#pragma once
#include <stdint.h>

#include "data_service.h"   // LinkData / FieldSource / VehicleState（-I lib/dashcore）
#include "link_frame.h"
#include "link_msg.h"
#include "link_phy.h"
#include "link_role.h"
#include "link_rx.h"
#include "link_time.h"
#include "link_tx.h"

// ============================================================
// 双板链路协议 v1 —— **应用层接线**（§1.2 ③ / §3 / §4 / §5）
//
// 这一层只做三件"协议 ↔ 本仓库"之间的事，别的都不做：
//   ① `Src`（0..3，§3 的 2 位编码）↔ `FieldSource`（data_service 的五档）互转 ——
//      ★ 就是 §3 表下那条自记缺口要的那一档（`FieldSource::Link`）；
//   ② **主板侧**：把"主循环里的快照"打包成 `DATA`（§1.2 ③：从快照发、不从回调发），
//      并按"跟随 VAN 0x824 到达"的节奏限速（§3 的 DATA 行）；
//   ③ **从板侧**：把收到的 `DATA` 解成 `LinkData`，交给 `VehicleDataService`。
//
// ★ 本层**不碰** UART、不碰 LVGL、不碰 millis()（时间由调用方传进来）——
//   于是它在宿主机上可测（`test_link_app.cpp` 把"发 → 收 → 喂进 data_service"
//   整条链串起来跑一遍，用的是 test/test_dashcore/fake_link_phy.h）。
//
// ★ 三条"别在这里做"的事（做了就违反 §1.2）：
//   · 别在 `LinkRx` 解出帧的当场顺手发帧 —— 收帧与发帧都只在主循环里；
//   · 别自己起定时器发 `DATA`：§3 明写"跟随 VAN 0x824 到达，**不另建定时器**"；
//   · 别把 `DATA` 的内容**缓存起来等以后发**：那会让"从快照发"退化成"从旧值发"。
// ============================================================

namespace dashlink {

// ------------------------------------------------------------
// ① 来源编码互转（§3 的 2 位 flags ↔ data_service 的 FieldSource）
// ------------------------------------------------------------
// 前四档数值刻意一一对应（link_msg.h 有静态对账用例）。Link（= 4）**编不进 2 位**
// ⇒ 它只出现在"本机作为从板"这一类场景里；§3 的 DATA.flags 是从板**收到**的东西，
// 与 Link 无关。
Src          fieldSourceToSrc(FieldSource f);
FieldSource  srcToFieldSource(Src s);

// 一帧 DATA 载荷 → LinkData（含量纲换算与每字段来源；rx_ms 由调用方给）。
LinkData unpackDataToLinkData(const DataMsg& m, uint32_t rx_ms);

// 打包：把"当前快照 + 每字段来源"做成 DATA 载荷。
//   · 四个标量按 §3/§link_msg.h 的量纲换算（rpm×8、车速÷2.56、温度+40，含钳制）；
//   · 来源位由 dataFlagsPack() 给（**别手写移位**，§3 的位号）；
//   · ★ Link 这一档**编不进** flags（只有 2 位）⇒ 按 Sim 编（见 .cpp 的说明）。
DataMsg packLinkData(const VehicleState& st, const DataSourceStatus& src);

// ------------------------------------------------------------
// ② 主板侧：DATA 的发送节奏（§3 的 DATA 行）
// ------------------------------------------------------------
// 契约原文：DATA **跟随 VAN 0x824 到达（≈79.7 Hz）**，**不另建定时器**。
// 实现方式：调用方把"这一份快照是什么时候的"（`VanSource::lastUpdateMs()` 就是
// 0x824 最后一次到达的时刻）传进来；本类只在它**变了**的时候发一帧。
//   · 为什么用"变了"而不是"到点了"：0x824 是**广播**进来的（没有定时器可跟）；主循环
//     一圈跑很多次，而 0x824 只有 ≈80 次/秒 ⇒ 这个判据天然把速率压到 VAN 的到达率上。
//   · 一圈里连收了好几帧 0x824（被 LVGL 拖慢过）时**只发一帧**：与 TickGen 的
//     "不补发突发"同一口径（§1.3：单次很短、可随时被打断）。
class DataSender {
 public:
  // 可选下限（0 = 不额外限速，契约口径就是"跟随 0x824"）。
  // ★ §1.1 的占空比表按 ≈79.7 Hz 算下来 A→B 只占 ~14% ⇒ **没有必要**降频。
  //   真要为别的目的降频（比如调试），用这个参数，别去改判断条件。
  void setMinIntervalMs(uint32_t ms) { mMinIntervalMs = ms; }
  void reset();

  // 该发吗？true ⇒ 已把 out 填好（调用方直接 enqueueFrame(Data, …) 即可）。
  //   now_ms      = 主循环当前毫秒
  //   snapshot_ms = **这一份快照的时刻**（0x824 的 lastUpdateMs；没有 VAN 时传 0）
  //   st/src      = 快照本身 + 每字段来源（§3 的 flags 从这里来）
  // ★ 本函数**不碰 PHY、不碰缓冲**：只算"该不该发/发什么"（§1.2 ③ 的形状）。
  bool due(uint32_t now_ms, uint32_t snapshot_ms, const VehicleState& st,
           const DataSourceStatus& src, DataMsg* out);

  uint32_t sent() const { return mSent; }
  uint32_t lastSentMs() const { return mLastSentMs; }

 private:
  // ★ `mHasSent` 是**显式**的"发过没有"，不用"mLastUpdateMs == 0"当哨兵：
  //   快照时刻 0 是**合法**的（`VanSource::last_update_ms` 初值就是 0，
  //   而 millis() 从复位起算、上电后第一帧 VAN 完全可能落在 0~20 ms 内）。
  //   拿 0 当哨兵会让"上电那一帧"被当成"还没发过"，于是限速窗口少判一次 ——
  //   这种错在板上只会表现为"偶尔多发一帧"，几乎不可能靠肉眼发现。
  bool     mHasSent = false;     // 有没有真的发出去过一帧
  uint32_t mLastUpdateMs = 0;    // 上一份**发出去的**快照的时刻
  uint32_t mLastSentMs   = 0;
  uint32_t mMinIntervalMs = 0;
  uint32_t mSent = 0;
};

// ------------------------------------------------------------
// ③ 从板侧：一帧收下来之后做什么（纯路由，不碰 UART）
// ------------------------------------------------------------
// 返回 true = 这一帧被"消化"了：
//   · TICK  → 喂 LinkTime（§4：算偏移、推三级超时）
//   · DATA  → 解成 LinkData 写进 out_data（调用方在 update() 之前 applyLinkData）
//   · 其它（HELLO/STATUS/EVENT）→ 返回 false，交给调用方（日志/角色对账）
// ★ 为什么 TICK/DATA 走这里而 STATUS/EVENT 不走：后者是"往日志里写一行"的事，
//   与数据层无关，留在 main.cpp 更清楚（也避免本层依赖 dash_log）。
bool handleInbound(const Frame& f, LinkTime* t, uint32_t now_ms, LinkData* out_data);

}  // namespace dashlink

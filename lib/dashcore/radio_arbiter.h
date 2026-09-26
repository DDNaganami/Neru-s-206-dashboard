#pragma once
#include <stdint.h>

// ============================================================================
// 射频仲裁：**BLE OBD ↔ ESP-NOW 链路**（2026-09-27 车上实测之后新增）
// ============================================================================
//
// 为什么有这一层（车上的硬证据，见 `docs/BLE-OBD.md` §7.5 与那条提交）：
//   ESP32-S3 只有**一套 2.4G 射频**，Wi-Fi（ESP-NOW 跑在它上面）与 BLE 由 IDF 的
//   共存仲裁**分时**。实测出来的现象非常干净：
//     · 链路 PHY **不启动** ⇒ BLE **一次就连上**（`connects=1`）；
//     · 链路 PHY 在跑     ⇒ BLE 永远 `status=13`(`BLE_HS_ETIMEOUT`)，八项（地址类型/
//       客户端状态/扫描残留/连接参数/共存偏好/配对/内存/发射功率）全排掉之后只剩它。
//   ⇒ 结论：不是"不可能共存"，是**默认调度下 BLE 拿不到建连要的那几毫秒**。
//
// ★★ 本层只做两件事，都是**纯逻辑**（不碰 Arduino/IDF，宿主机可编、有用例钉住）：
//
//   ① `LinkStartGate` —— **开机顺序**：先让 BLE 建连，再启链路 PHY。
//      为什么这条最便宜：BLE 建连失败的那几秒**恰好是链路还没起来的那几秒**，
//      把两者错开就够了（不需要动任何射频参数）。
//      ★ **硬上限**：超过 `kWaitMaxMs` 一律开闸 —— 那条链路是仪表的**主命脉**，
//        绝不允许"OBD 连不上 ⇒ 从板永远没数据"。
//
//   ② `RadioArbiter` —— **运行期优先权**：BLE 需要建连时，把共存偏好临时切给 BT；
//      但它有**硬上限 + 冷却**，所以最坏情况是"链路被压 8 秒，然后至少 30 秒不被打扰"，
//      而不是"BLE 永远抢下去"。
//      ★ 为什么要有上限：`busy` 可以是**分钟级**的（ELM327 找不到/关机时，状态机一直
//        在扫+退避重连）。没有上限的话，一个连不上的 OBD 会**永久**压低链路质量 ——
//        那是拿主命脉换一个可选功能，方向反了。
//
// ★ 设备侧怎么用（见 `obd_transport_ble.cpp` 的 `tick()`）：
//     ```cpp
//     const bool hold = arb_.update(now_ms, busy_for_radio(), ready());
//     if (hold != last_hold_) esp_coex_preference_set(hold ? ESP_COEX_PREFER_BT
//                                                         : ESP_COEX_PREFER_BALANCE);
//     ```
//   `busy_for_radio()` = "**扫到过对端** && 还没 ready" —— 没扫到过对端（台面上根本
//   没有诊断头）时**一次都不抢**，那是纯白抢。
// ============================================================================

namespace dashcore {

// ---------------------------------------------------------------------------
// ① 链路 PHY 的启动闸门（主板 + `OBD_BLE` 的构建才有意义；别处恒 true）
// ---------------------------------------------------------------------------
struct LinkStartGate {
  // BLE 建连窗口的上限。取值依据：
  //   · `ObdTransportBle` 里 `setConnectTimeout(15)` 秒 ⇒ 给足一次完整尝试；
  //   · 同时它是"从板还能忍多久"的账：从板上 `tick_age > 3s` 就回退 Sim 并挂
  //     "数据不可信"角标 ⇒ 这 15 秒里从板是**模拟数据状态**（如实记在文档里，
  //     不假装它是零代价）。
  static constexpr uint32_t kWaitMaxMs = 15000u;

  // 返回 true = 现在可以启动链路 PHY 了。
  //   · 没有 BLE 这一路（`OBD_BLE=0`：有线档 / 从板 / pcpreview）⇒ **立刻开**，
  //     与改之前逐字节相同（那些构建连这个函数都不会调）。
  //   · BLE 已 ready（连上 + 特征齐）⇒ 开。
  //   · 等够 `kWaitMaxMs` ⇒ 开（仪表优先，不再等 OBD）。
  static bool open(bool obd_ble, bool obd_ready, uint32_t waited_ms) {
    if (!obd_ble) return true;
    if (obd_ready) return true;
    return waited_ms >= kWaitMaxMs;
  }
};

// ---------------------------------------------------------------------------
// ② 射频优先权（有状态：每圈调一次 `update()`）
// ---------------------------------------------------------------------------
class RadioArbiter {
 public:
  // 一次连续占用的上限。为什么是 8 秒：
  //   BLE 建连（扫描一轮 2s + 一次 connect）在这个量级；再长就不是"抢时隙"，
  //   而是"把链路停掉"了。
  static constexpr uint32_t kHoldMaxMs = 8000u;
  // 让出之后的冷却：这段时间里**即使还在忙也不抢**，把射频还给链路。
  static constexpr uint32_t kCooldownMs = 30000u;
  // 连上之后再多压一会儿：服务发现 + 订阅 CCCD + 第一条指令要几个来回，
  // 这一段给足优先权，省得"连上了又因为拿不到时隙而掉线"。
  static constexpr uint32_t kGraceAfterOkMs = 2000u;

  // 每圈调一次。返回 true = **现在**应当把共存偏好切给 BT。
  //   `busy` = BLE 正在找/正在连（见文件头：要"扫到过对端"才算忙）
  //   `ready`= 连上且特征齐（= `ObdTransport::connected()` 那一路的最终态）
  bool update(uint32_t now_ms, bool busy, bool ready) {
    if (holding_) {
      if (ready) {
        // ★ 只在**进入占用之后第一次**看到 ready 时打点：之后每圈刷新会把
        //   `now - ready_at` 永远算成 0，grace 就永远不过期（踩过这个坑）。
        if (!ready_seen_) {
          ready_seen_ = true;
          ready_at_   = now_ms;
        }
        if ((uint32_t)(now_ms - ready_at_) >= kGraceAfterOkMs) {
          release(now_ms);
          ++released_ok_;
          return false;
        }
      }
      if ((uint32_t)(now_ms - hold_since_) >= kHoldMaxMs) {
        release(now_ms);
        ++capped_;          // ★ 被上限掐断的次数：这个数在体检行里可见
        return false;
      }
      return true;
    }

    ready_seen_ = false;    // 不占用时清掉，下次占用重新计时
    // ★★ 冷却判断必须带一个**显式的布尔** `cooling_`，不能拿 `cool_until_ == 0` 当
    //   "没在冷却"：`now_ms` 一大（`millis()` 回绕之后就是这种情况），
    //   `(int32_t)(now_ms - 0)` 是**负数** ⇒ 会被误判成"正在冷却"⇒ **一次都不抢**。
    //   这条不是推演出来的：`test_arb_rollover_safe` 第一次跑就抓到了它。
    if (cooling_) {
      if ((int32_t)(now_ms - cool_until_) < 0) return false;   // 还在冷却中
      cooling_ = false;                                        // 冷却结束
    }
    if (!busy) return false;
    holding_    = true;
    hold_since_ = now_ms;
    ++windows_;
    return true;
  }

  bool     holding() const { return holding_; }
  uint32_t windows() const { return windows_; }        // 一共抢过几次
  uint32_t capped() const { return capped_; }          // 被 8 秒上限掐断几次
  uint32_t releasedOk() const { return released_ok_; } // 因"连上了 + grace 到"正常让出几次

 private:
  void release(uint32_t now_ms) {
    holding_    = false;
    cooling_    = true;                                   // ★ 见 update() 里那条说明
    cool_until_ = (uint32_t)(now_ms + kCooldownMs);
  }

  bool     holding_   = false;
  bool     cooling_   = false;   // 是否处于冷却期（**不用 `cool_until_ == 0` 判**）
  bool     ready_seen_ = false;
  uint32_t hold_since_ = 0;
  uint32_t cool_until_ = 0;
  uint32_t ready_at_   = 0;
  uint32_t windows_    = 0;
  uint32_t capped_     = 0;
  uint32_t released_ok_ = 0;
};

}  // namespace dashcore

#pragma once
#include "obd_transport.h"

// ★ 只有显式定义 `OBD_BLE` 的构建才编这一层（见 platformio.ini 的主板 `-now` 档）。
//   这样别的 env 既不需要 NimBLE 依赖，也不会因为找不到 <NimBLEDevice.h> 而编不过。
#if defined(ARDUINO) && defined(OBD_BLE)

#include <NimBLEDevice.h>
#include "radio_arbiter.h"   // ★★ 射频仲裁（BLE ↔ ESP-NOW 共存策略，见文件头那段）

// ============================================================================
// ObdTransportBle —— 把 `ObdTransport` 接到一个 BLE OBD 诊断头上
// ============================================================================
//
// 目标设备（车主手上那个；**笔记本侧**已实测，见 docs/BLE-OBD.md）：
//   广播名 `OBDBLE`、地址 `AABBCC122233`（克隆方案默认值，换一个头就会变），GATT：
//     服务 `0000FFF0`：
//       `0000FFF1` props=0x12（读 + **通知**）   ← 诊断头 → 中心，**回答从这里来**
//       `0000FFF2` props=0x0C（**写** + 无响应写）← 中心 → 诊断头，**指令往这里写**
//
// ★ 这一层只做搬运：问答状态机、PID 轮询表、应答解析全在 `ObdSource` 里，一行没改。
//   笔记本上实测出来的那几条纪律**只影响这里**：
//     1. **回答是分片的**（实测 `ATZ` 被切成 1+13 两片）⇒ 通知一片一片进环；
//     2. **它会自己掉线**（空闲断开）⇒ 必须重连（`tick()` 里那个状态机）;
//     3. **超时要给宽**（KWP FAST，`0100` 前几秒只回 `SEARCHING...`）——
//        那是 `ObdSource` 的超时值，不在这层。
//
// ★★ 线程模型（最容易写错的一条）：
//   NimBLE 的回调跑在**它自己的任务**里，`ObdSource::tick()` 跑在主循环。
//   ⇒ 生产者在回调里**只把字节塞进单生产者/单消费者环**（无锁：volatile 头尾 + 序号，
//     与 `lib/dashcore/van_edge_queue.h` 同一条纪律）；消费只在 `available()/read()`。
//     **回调里绝不打印、绝不分配、绝不碰 LVGL**。
//
// ★★ 射频共存（一条要盯着的风险，本层解决不了）：
//   板间链路走 **ESP-NOW**（同一个 2.4G 射频），而 BLE 中心也要用射频。
//   两者能否稳当共存**没有实测过** —— 上车前要看链路侧那几行
//   （主板的 `espnow: tx_fail/done_fail`、从板的 `rx_overflow` 与 `tick_age`）有没有变差。
//
// ★★ 射频共存（2026-09-27 车上实测：**这个头与 ESP-NOW 抢射频**，本层只做两件事，
//   策略全在 `lib/dashcore/radio_arbiter.h` 里、由 native 用例钉住）：
//     ① 运行期：BLE 建连窗口内把共存偏好临时切给 BT（一次 ≤8s，让出后冷却 ≥30s）；
//     ② 开机顺序：主板**先让 BLE 建连、再启链路 PHY**（闸门在 `src/main.cpp`，
//        用的是同一个头里的 `LinkStartGate`）。
//   仍然没解决的（如实记着）：两者稳态共存时的**链路质量**没有被量化过 ——
//   下一单上车要盯 `link:` / `meas rx:` 那几行有没有变差。
//
// ★ API 出处：NimBLE-Arduino **2.5.1**（`libdeps/.../NimBLE-Arduino/src/`）。
//   这一版是 2.x 的**新回调风格**：`NimBLEScanCallbacks::onDiscovered/onResult/onScanEnd`，
//   `NimBLEClientCallbacks::onConnect/onDisconnect`。别按 1.x 的
//   `NimBLEAdvertisedDeviceCallbacks` 写（那个类在 2.x 里已经没有了）。
class ObdTransportBle : public ObdTransport {
public:
  static constexpr uint16_t kRingBytes = 256;   // ELM327 一条回答几十字节，够宽裕
  static constexpr const char* kServiceUuid = "0000fff0-0000-1000-8000-00805f9b34fb";
  static constexpr const char* kNotifyUuid  = "0000fff1-0000-1000-8000-00805f9b34fb";
  static constexpr const char* kWriteUuid   = "0000fff2-0000-1000-8000-00805f9b34fb";

  ObdTransportBle() = default;

  // ---- ObdTransport ----
  bool start() override;
  bool connected() const override { return conn_; }

  void write(const char* s) override;
  void write(char c) override;

  int available() override { return (int)(uint16_t)(head_ - tail_); }
  int read() override {
    const uint16_t t = tail_;
    if (t == head_) return -1;
    const char c = (char)ring_[t % kRingBytes];
    tail_ = (uint16_t)(t + 1);
    return (uint8_t)c;
  }

  // 主循环里调：推进"扫描 → 连接 → 掉线后重连"（非阻塞）
  void tick(uint32_t now_ms);

  // 诊断用（进日志）
  const char* stateName() const;
  // ★ `start()` 成功了吗（NimBLE 起来了）。**启动闸门要用它**：如果 `start()` 失败
  //   （协议栈没起来），那"等 BLE 连上"就是**白等** —— 闸门应当立刻开，
  //   否则上电会平白多出 15 秒的"从板没数据"。
  bool     started() const { return started_; }
  bool     ready() const { return conn_ && notify_ != nullptr && write_ != nullptr; }
  uint32_t dropped() const { return dropped_; }
  uint32_t connects() const { return connects_; }
  // ★ 车上排查用:进 connecting 分支几次 / 真的发起 connect 几次
  uint32_t csAttempts() const { return cs_attempts; }
  uint32_t csCalls() const { return cs_calls; }
  const char* peerText() const { return peer_[0] ? peer_ : "-"; }

  // ---- 射频仲裁（诊断用；体检行里打出来）----
  // `radioHeld()` = **现在**射频优先权在 BLE 手上（真去调过 `esp_coex_preference_set`）。
  bool     radioHeld() const { return arb_.holding(); }
  uint32_t radioWindows() const { return arb_.windows(); }   // 抢过几次
  uint32_t radioCapped() const { return arb_.capped(); }     // 被 8s 上限掐断几次
  // "现在需要射频吗"：★ **扫到过对端** 且还没 ready。
  //   为什么要 `peer_valid_`：台面上根本没有诊断头时（peer 从没扫到），
  //   状态机也会一直在扫+退避重连 ⇒ 那种"忙"抢射频是**纯白抢**，会平白压低链路。
  bool     wantsRadio() const { return peer_valid_ && !ready(); }

private:
  friend class ObdBleScanCb;
  friend class ObdBleClientCb;

  void onDiscovered(const NimBLEAdvertisedDevice* d);   // 回调 → 记地址 + 停扫
  void onDisconnected();                                // 回调 → 清句柄（掉线后它们就失效了）
  void pushBytes(const uint8_t* d, size_t n);           // 只在回调里调
  bool connectNow();
  void applyRadioArbitration(uint32_t now_ms);          // 射频优先权的**过渡**处理
  void setCoexPreference(bool ble_first);               // 真去调 IDF（过渡时才调）

  NimBLEScan*   scan_   = nullptr;
  NimBLEClient* client_ = nullptr;
  NimBLERemoteCharacteristic* notify_ = nullptr;
  NimBLERemoteCharacteristic* write_  = nullptr;

  volatile uint16_t head_ = 0, tail_ = 0;
  volatile uint8_t  ring_[kRingBytes] = {0};

  bool          started_ = false;
  volatile bool conn_    = false;
  bool          want_peer_ = false;      // 扫到目标了，下一拍去连
  uint32_t dropped_  = 0;
  uint32_t connects_ = 0;
  uint32_t cs_attempts = 0;
  uint32_t cs_calls = 0;
  uint32_t boot_ms_  = 0;
  uint32_t last_try_ms_ = 0;
  uint32_t backoff_ms_  = 0;             // 退避：500ms → … → 8s
  // ★★ 存**整个 `NimBLEAddress`**（含地址类型），不是地址字符串 ——
  //   实测：`aa:bb:cc:12:22:33` 这种是**随机静态地址**，拿字符串重建成
  //   `BLE_ADDR_PUBLIC` 去连**永远连不上**，而日志里只会看到 `conn=0`，
  //   看起来像"设备不在/被占着"。`getAddress()` 回来的对象类型是对的。
  NimBLEAddress peer_addr_{};
  bool          peer_valid_ = false;
  char     peer_[20] = {0};              // 仅供日志打印

  // ★★ 射频仲裁状态机（策略见 radio_arbiter.h，这里只存实例与"上次有没有占用"）
  dashcore::RadioArbiter arb_{};
  bool     coex_hold_     = false;       // 上一次同步给 IDF 的偏好（只在过渡时改）
  uint32_t coex_bt_err_   = 0;           // 第一次切 BT 的 `esp_err_t`（0 = 成功）
  uint32_t coex_bal_err_  = 0;           // 第一次切回 BALANCE 的 `esp_err_t`

  static ObdTransportBle* s_self_;
};

#endif  // ARDUINO && OBD_BLE

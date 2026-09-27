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
  // ★★★ 2026-09-28：异步连接的**真错误码**（这是本轮改造的全部目的）。
  //   异步下 `connect()` 一发起就返回，失败必然走 `onConnectFail(reason)`，
  //   所以这两个数才是可归因的：以前只能看到库补写的那个假的 13。
  uint32_t connectFails() const { return connect_fails_; }
  int      lastFailReason() const { return last_fail_reason_; }
  const char* peerText() const { return peer_[0] ? peer_ : "-"; }

  // ---- 射频仲裁（诊断用；体检行里打出来）----
  // `radioHeld()` = **现在**射频优先权在 BLE 手上（真去调过 `esp_coex_preference_set`）。
  bool     radioHeld() const { return arb_.holding(); }
  // ★ 2026-09-27 深夜（分工 v2）：射频占用上限**按角色可调** —— 从板跑 BLE 时被压住的
  //   是"它收主板的 TICK"（压过 3 秒左屏就掉进"数据不可信"）⇒ 从板设 4000ms。
  //   执行点见 `main.cpp` 里 `g_obd_ble.start()` 之后那一处。
  void     setRadioHoldMaxMs(uint32_t ms) { arb_.setHoldMaxMs(ms); }
  uint32_t radioHoldMaxMs() const { return arb_.holdMaxMs(); }
  uint32_t radioWindows() const { return arb_.windows(); }   // 抢过几次
  uint32_t radioCapped() const { return arb_.capped(); }     // 被 8s 上限掐断几次

  // ★★★ 2026-09-28：**射频抑制开关**（串口 `o` 切换）。
  //   用途只有一个：**判"横纹/上移是不是射频抢占造成的"** —— 敲一个字符就能把
  //   射频占用这个**唯一变量**开/关，在屏前直接 A/B，不用重刷固件
  //   （刷机几十秒 + 打断现场观察，而且判据和变量一起动就分不清）。
  //   ★ 语义：抑制期间 **不再扫、不再连、占用的窗口按超时自然释放**；
  //     **已经连上的连接不受影响**（抑制只挡"尝试"这条路，见 .cpp 的 tick()）。
  //     解除后按原来的退避节奏继续，不需要重启。
  void setInhibited(bool on) { inhibited_ = on; }
  bool inhibited() const { return inhibited_; }
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
  // ★★★ 2026-09-28：连上之后把服务/特征/订阅配齐。**从 `tick()` 里调，不在回调里做**
  //   —— `getService()` / `subscribe()` 都要等对端应答（走 `taskWait` 那一族），
  //   在 NimBLE 任务上下文里同步等容易把自己等死。
  //   返回 true ⇒ `ready()` 成立；false ⇒ 调用方断开重来。
  bool attachCharacteristics();
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
  // ★★★ 2026-09-28：**停扫时刻**（只由"我们主动 `scan_->stop()`"那一刻置位，0 = 没在等）。
  //   用途见 .cpp 里"停扫之后等够再连"那一段 —— 那道闸门**必须有自己的时间戳**，
  //   绝不能借 `last_try_ms_`：它在函数开头刚被赋值，差值恒 0 ⇒ 闸门永远成立
  //   ⇒ **connect() 一次都发不出去**（2026-09-27 就是这么栽的，然后闸门被整段删掉、
  //   原始问题又回来了）。这条注释就是防止第三次踩同一处。
  uint32_t scan_stop_ms_ = 0;
  // ★★★ 2026-09-28：**重试退避上限**（原来是硬编码的 8000，改名为常量并放大到 60s）。
  //
  //   为什么必须放大 —— 这是"横纹"的根因，有**现场 A/B 实测**：
  //     车主报"副板横纹 + 图案上移"。用串口 `o` 把**射频占用**这一个变量关掉
  //     （`setInhibited()`），横纹**立刻消失** ⇒ 确认不是显示/时钟问题，
  //     而是 BLE 的射频占用抢走了**弹跳缓冲填充**的时间片
  //     （bounce 模式下 CPU 要从 PSRAM 搬 450KB/帧；CPU 被挤 ⇒ 某一行来不及填
  //      ⇒ 那一行吐旧数据 ⇒ **会移动的横纹**）。
  //
  //   ★ 时序账（为什么 8s 上限太高）：撞一次的成本不是那 17ms，而是**仲裁器的整段
  //     占用**：`kHoldMaxMs`（从板设 4000）**+ 之后 `kCooldownMs = 30000` 的冷却**
  //     ⇒ 最长每 ~34 秒就要压一次射频、每次约 4 秒。而且 `wantsRadio()` 在
  //     "扫到过对端但没就绪"期间**恒为 true**，所以它**自己不会停**。
  //     ⇒ 屏上就是"周期性的一阵横纹"，与车主看到的现象一致。
  //
  //   ★ 为什么取 60s（而不是继续 8s 或取 30s）：退避上限应当**≥ 仲裁器冷却**
  //     （30s），否则退避还没走完、仲裁器已经又允许下一次占用 ⇒ 上限形同虚设。
  //     60s = 30s 冷却的 2 倍，留一倍余量 ⇒ 稳态下射频占空比 ≈ 4s/64s ≈ **6%**，
  //     对弹跳缓冲的影响降到可忽略。
  //
  //   ★ 代价（写清楚，别以为是免费的）：**诊断头真的插上时，最多要多等 60 秒**
  //     才连上。对"车已经打着、数据慢变量"的场景可以接受；
  //     连上一次之后 `backoff_ms_` 会被立刻清零（`connectNow()` 成功那条），
  //     所以**正常使用不受影响，只在一直连不上时才退到 60s**。
  static constexpr uint32_t kRetryBackoffMaxMs = 60000u;

  // ★★★ 2026-09-28：**射频抑制**（串口 `o` 切换）—— 用来判"横纹是不是射频抢占造成的"。
  //   置位后 tick() 在一切射频动作之前早退 ⇒ BLE 不再扫、不再连、不再占射频。
  //   为什么要有它而不用"重刷一版关掉 BLE 的固件"：刷机几十秒 + 打断现场观察，
  //   而且判据与变量一起动就分不清；敲一个字符开/关才能做真正的 A/B。
  bool          inhibited_ = false;

  // ==========================================================================
  // ★★★ 2026-09-28：**异步连接**（`asyncConnect = true`）要的三个状态 + 一个超时
  //
  //   为什么改成异步（一句话）：同步 `connect()` 阻塞在 `NimBLEUtils::taskWait()`，
  //   实测 16~17ms 就返回 false、随后库补写 `BLE_HS_ETIMEOUT(13)` 当挡箭牌
  //   ⇒ `status` 与 `getLastError()` 都拿不到真错误码（docs/BLE-OBD.md §12.1/§12.4）。
  //   异步把那条路整个绕开：失败进 `onConnectFail(reason)`，**那才是真错误码**。
  //
  //   ★★ `connectNow()` 的**返回值语义已变**，调用方要看清：
  //      旧 = "连上了"；新 = "**请求已受理**"（连接还在进行中）。
  //      所以不能再拿它去 `++connects_`（那会变成"发一次请求算一次成功"）；
  //      成功与否一律看 `ready()` / `conn_`（由回调设置）。
  // ==========================================================================
  bool     connect_pending_ = false;   // 已发起、还没等到 onConnect / onConnectFail
  uint32_t connect_started_ms_ = 0;
  // ★★ 上一拍是否已就绪 —— 用来把 `connects_` 记成"**成功连上过几次**"而不是"每拍+1"。
  //   `ready()` 在连上期间每拍都成立，没有这个边沿判据 `connects_` 会涨到几百。
  bool     was_ready_ = false;
  // 发起后等多久算"没有任何回调"。取 15s（与库原本那个连接超时同量级）。
  // ★ 走到这条超时本身就是判据：`onConnectFail` 也没来 ⇒ 问题在库的前置检查
  //   或控制器没回事件，**不在射频**。
  static constexpr uint32_t kAsyncConnectTimeoutMs = 15000u;
  // 诊断用（进 1 Hz 状态行）：异步失败次数与**最近一次的真实 reason**。
  //   ★ reason 是本次改造的全部目的 —— 以前只能看到那个假的 13。
  uint32_t connect_fails_    = 0;
  int      last_fail_reason_ = 0;

  // 停扫之后等多久才允许 connect。控制器收尾是几十毫秒量级，取 600ms = 10 倍余量。
  static constexpr uint32_t kPostScanSettleMs = 600u;
  // ★★★ 2026-09-28：连败这么多次就**丢掉地址、重新扫**（地址新鲜度的补丁）。
  //   理由见 .cpp 里那段 —— `peer_addr_` 原本是开机扫一次就再也不刷新，
  //   换头/对端重启后地址一变，就变成拿过期地址盲撞（控制器当场拒、17ms 返回）。
  static constexpr uint32_t kRescanAfterFails = 5u;
  // ★★ 2026-09-28：**扫描上限**（连不上就别一直扫 —— 那会把射频从 ESP-NOW 那边抠走，
  //   见 obd_transport_ble.cpp 里那个分支的说明）。扫到对端时两个量都会被复位。
  static constexpr uint8_t  kScanTriesFast = 10u;        // 先快速试 10 轮（≈1 分钟）
  static constexpr uint32_t kScanSlowMs    = 300000u;    // 之后每 5 分钟试一次
  uint8_t  scan_tries_        = 0;
  uint32_t last_slow_scan_ms_ = 0;
  // ★★ 存**整个 `NimBLEAddress`**（含地址类型），不是地址字符串 ——
  //   实测：`aa:bb:cc:12:22:33` 这种是**随机静态地址**，拿字符串重建成
  //   `BLE_ADDR_PUBLIC` 去连**永远连不上**，而日志里只会看到 `conn=0`，
  //   看起来像"设备不在/被占着"。`getAddress()` 回来的对象类型是对的。
  NimBLEAddress peer_addr_{};
  bool          peer_valid_ = false;
  int           peer_rssi_  = 0;      // ★ 扫描到那一刻的 RSSI(dBm),判弱链路用
  char     peer_[20] = {0};              // 仅供日志打印

  // ★★ 射频仲裁状态机（策略见 radio_arbiter.h，这里只存实例与"上次有没有占用"）
  dashcore::RadioArbiter arb_{};
  bool     coex_hold_     = false;       // 上一次同步给 IDF 的偏好（只在过渡时改）
  uint32_t coex_bt_err_   = 0;           // 第一次切 BT 的 `esp_err_t`（0 = 成功）
  uint32_t coex_bal_err_  = 0;           // 第一次切回 BALANCE 的 `esp_err_t`

  static ObdTransportBle* s_self_;
};

#endif  // ARDUINO && OBD_BLE

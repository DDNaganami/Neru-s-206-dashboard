#include "obd_transport_ble.h"

#if defined(ARDUINO) && defined(OBD_BLE)

#include "dash_log.h"
// ★ 射频共存那两行要用 IDF 的 API。**头文件叫 `esp_coexist.h`**（不是 `esp_coex.h`）
//   —— 实测写错名字直接 `fatal error: esp_coex.h: No such file or directory`。
//   真实路径：`framework-arduinoespressif32-libs/esp32/include/esp_coex/include/esp_coexist.h`
//   （函数 `esp_coex_preference_set(esp_coex_prefer_t)`、枚举 `ESP_COEX_PREFER_BALANCE`）。
#if defined(ARDUINO)
#include <esp_coexist.h>
#endif

ObdTransportBle* ObdTransportBle::s_self_ = nullptr;

// ============================================================================
//  两个回调壳 —— NimBLE 2.x 是**回调类**风格（不是 1.x 的裸函数指针）
// ============================================================================
// ★ 它们必须定义在 `ObdTransportBle` **之外**：`setScanCallbacks()` /
//   `setClientCallbacks()` 收的是基类指针，而且默认 `deleteCallbacks=true`
//   （库自己 new/delete 一份）。所以这里用小壳转发到单例。
// ★ 两个壳里**只做搬运**：不打印、不分配。打印会把 NimBLE 的任务拖住，
//   而那个任务同时还管着连接——正是我们要避免的"链路卡死"。
class ObdBleScanCb : public NimBLEScanCallbacks {
public:
  void onDiscovered(const NimBLEAdvertisedDevice* d) override {
    if (ObdTransportBle::s_self_ && d) ObdTransportBle::s_self_->onDiscovered(d);
  }
  // onResult / onScanEnd 用不上：认设备靠 onDiscovered 里那条服务 UUID 判据，
  // 扫到就停扫（不必等整轮扫完）。
};

class ObdBleClientCb : public NimBLEClientCallbacks {
public:
  void onConnect(NimBLEClient* c) override {
    (void)c;
    if (ObdTransportBle::s_self_) ObdTransportBle::s_self_->conn_ = true;
  }
  // ★★ 连接**失败的原因码**只有这里能拿到（`connect()` 只回 bool）。
  //   2026-09-27 实测：板子在车上扫描能看到诊断头（`peer=` 有了），但 `connect()`
  //   **立刻**失败（重试间隔 ~1s，而连接超时设的是 8s ⇒ 不是超时，是对端拒绝）。
  //   把原因码打出来才能区分这几种：
  //     · `BLE_ERR_CONN_ESTABLISHMENT`(0x3E) → 对端没应/不在
  //     · `BLE_ERR_UNK_CONN_ID`(0x02)        → 连接已不存在（时序问题）
  //     · `BLE_ERR_AUTH_FAIL`(0x05)          → 要配对/绑定（**本头可能就是这种**：
  //        它被 Windows 配对过 ⇒ 可能要求加密链路，而我们是"裸连"）
  //   ⇒ 这就是当初"为什么必须把错误码打出来"的原因。
  void onConnectFail(NimBLEClient* c, int reason) override {
    (void)c;
    dash_logf("obd-ble: onConnectFail reason=0x%02X\n", (unsigned)reason);
  }
  // 掉线原因同样有用（空闲掉线 vs 远端主动断）
  void onDisconnect(NimBLEClient* c, int reason) override {
    (void)c;
    dash_logf("obd-ble: onDisconnect reason=0x%02X\n", (unsigned)reason);
    if (ObdTransportBle::s_self_) ObdTransportBle::s_self_->onDisconnected();
  }
};

// ---------------------------------------------------------------------------
const char* ObdTransportBle::stateName() const {
  if (!started_) return "off";
  if (ready())   return "ready";
  if (conn_)     return "connected-no-chars";
  if (want_peer_) return "connecting";
  return "scanning";
}

void ObdTransportBle::onDisconnected() {
  conn_    = false;
  notify_  = nullptr;
  write_   = nullptr;
}

// 环：单生产者（NimBLE 任务）/ 单消费者（主循环）。满了**丢新字节并计数**，
// 不覆盖旧数据 —— 覆盖会让"半条回答"拼到另一条上，解出来的 PID 值是错的。
void ObdTransportBle::pushBytes(const uint8_t* d, size_t n) {
  if (d == nullptr) return;
  const uint16_t used   = (uint16_t)(head_ - tail_);
  const uint16_t free_n = (uint16_t)(kRingBytes - used);
  if (n > free_n) { dropped_ += (uint32_t)(n - free_n); n = free_n; }
  for (size_t i = 0; i < n; ++i) {
    ring_[head_ % kRingBytes] = d[i];
    head_ = (uint16_t)(head_ + 1);
  }
}

// 扫到一个广播：只认**服务 UUID**（名字是广播里的可选字段，不牢靠）。
void ObdTransportBle::onDiscovered(const NimBLEAdvertisedDevice* d) {
  if (d == nullptr || want_peer_) return;
  if (!d->haveServiceUUID()) return;
  if (!d->isAdvertisingService(NimBLEUUID(kServiceUuid))) return;
  // ★★ 存**整个地址对象**（含地址类型）。实测教训：`aa:bb:cc:12:22:33` 是
  //   随机静态地址，若存成字符串再按 `BLE_ADDR_PUBLIC` 重建 ⇒ 连接永远失败，
  //   而日志只有 `conn=0`（看起来像"设备不在/被手机占着"），极易误判。
  peer_addr_  = d->getAddress();
  peer_valid_ = true;
  snprintf(peer_, sizeof(peer_), "%s", peer_addr_.toString().c_str());
  want_peer_ = true;
  if (scan_) scan_->stop();     // 找到就停，别再占射频
}

// ---------------------------------------------------------------------------
bool ObdTransportBle::start() {
  if (started_) return true;
  s_self_ = this;

  NimBLEDevice::init("206dash");              // 只当中心：不建服务、不广播
  // ★★ 发射功率拉满（2026-09-27 车上加）。线索：同一时刻**笔记本**（Intel AX210，
  //   真正的天线）在 −81dBm 下还能一连就上；而 ESP32-S3 用的是**板载小天线**，
  //   收发都弱一截。我们那个头也是小天线 ⇒ 双方都弱 ⇒ 表现成
  //   "扫描收得到广播（最低速率的广播包收得到），但建连一直 `status=13` 超时"
  //   （建连要双向可靠握手，对链路预算敏感得多）。
  //   ★★ 这里踩过一个坑，写下来：**有两个同名很像的函数，参数口径完全不同** ——
  //        `setPower(int8_t dbm, …)`        收的是 **dBm 数值**（9 就是 +9dBm）
  //        `setPowerLevel(esp_power_level_t, …)` 收的才是**枚举**（`ESP_PWR_LVL_P9`）
  //      我第一版把**枚举传给了收 dBm 的那个** ⇒ 9 被当成 dBm 去换算枚举、
  //      越界 → **空指针崩溃**（`Guru Meditation: LoadProhibited`、`EXCVADDR: 0x38`、
  //      25 秒里重启 23 次）。
  //   ★ 放在 `init()` **之后**（控制器就绪了再设功率，顺序更稳）。
  //   ★ 这只是把链路预算往好里推，不保证够；真正的解法是**让两块设备离近点**
  //     （诊断头在 OBD 座、板子在别处的话，挪近 30cm 能换来 10dB 量级）。
  NimBLEDevice::setPower(9);
  // ★★ 允许**配对/绑定**（2026-09-27 车上加，验证"这个头只认配过的中心"这条假设）：
  //   那个诊断头被 Windows 配对并使用过（笔记本侧实测连上过并读到了 FFF0/FFF1/FFF2），
  //   而 BLE 从机常见两种脾气：
  //     · **只接受已绑定中心的连接**（裸连会被忽略 ⇒ 表现正是我们看到的 `status=13` 超时）；
  //     · 或者必须先加密链路才让访问特征。
  //   两句都设上（`bonding=true` 允许绑定、`sc=true` 走 LE Secure Connections），
  //   设备侧若要配对就会触发配对流程；不要就无副作用（`mitm=false` ⇒ Just Works，无 PIN）。
  //   ★ 实测口径：若这次 `connects` 变成 1 → 就是这条；若仍 `status=13` → 再排它。
  NimBLEDevice::setSecurityAuth(true /*bonding*/, false /*mitm*/, true /*sc*/);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  // ★★ 射频共存（2026-09-27 车上实测之后改定 —— **上一版"一直 BALANCE"是不够的**）：
  //   车上的硬证据：链路 PHY **不启动** ⇒ BLE 一次就连上（`connects=1`，`ready`）；
  //   链路在跑 ⇒ 永远 `status=13(BLE_HS_ETIMEOUT)`。地址类型 / 客户端状态 / 扫描残留 /
  //   连接参数 / 共存偏好 / 配对 / 内存(10KB 与 29KB 表现一致) / 发射功率八项全排掉
  //   之后，只剩"**射频被 ESP-NOW 抢掉**"这一条。
  //   ⇒ 现在不再是"开机设一次 BALANCE 就完事"，而是**按仲裁状态机在过渡时切**：
  //        · 静止态 = `BALANCE`（两边均衡；链路是仪表的主命脉）；
  //        · BLE 真要建连的那几秒 = `ESP_COEX_PREFER_BT`，**一次最多 8s**，
  //          让出后**至少冷却 30s**（见 `radio_arbiter.h` 里那两个常量与理由）。
  //   ★ 开机这里先落到静止态；真正的切换在 `tick()` 的 `applyRadioArbitration()`。
  //   ★ 顺带把 `esp_err_t` 记下来（这个 API 可能返回"不支持"）——日志里能看到。
#if defined(ARDUINO)
  setCoexPreference(false);
#endif
  client_ = NimBLEDevice::createClient();
  if (client_ == nullptr) return false;
  client_->setClientCallbacks(new ObdBleClientCb(), true);
  client_->setConnectTimeout(15);             // 秒（8 → 15：读数头慢，别过早放弃）
  // ★★ 连接参数**显式给全 6 个**（2026-09-27 车上：`status=13` = `BLE_HS_ETIMEOUT`
  //   ⇒ "连接请求发出去了、但对端没在超时内被连上"）。
  //   后两个参数是**发起连接时用的扫描间隔/窗口** —— 它们才是关键：
  //   NimBLE 默认拿主扫描的那组（100ms 间隔 / 80ms 窗口）去连，节奏偏密；
  //   给一组"窗口=间隔"的快速扫描（40ms/40ms）+ 标准连接间隔（30~50ms）+
  //   监督超时 4 秒，这对一个慢速 K 线适配器的射频前端友好得多。
  //   ★ 单位：`itvl`/`scan*` 是 1.25ms？—— 不：NimBLE 这里 **minInterval/maxInterval
  //     是 1.25ms 单位**、`timeout` 是 10ms 单位，而 `scanInterval/scanWindow` 也是
  //     1.25ms 单位；下面 24/40 = 30/50ms、400 = 4s、32/32 = 40ms/40ms。
  client_->setConnectionParams(24, 40, 0, 400, 32, 32);
  client_->setConnectRetries(3);

  scan_ = NimBLEDevice::getScan();
  if (scan_ == nullptr) return false;
  scan_->setScanCallbacks(new ObdBleScanCb(), true);
  scan_->setInterval(100);
  scan_->setWindow(80);
  scan_->setActiveScan(false);                // 被动扫描：够认出服务 UUID 了
  scan_->setMaxResults(8);

  started_  = true;
  boot_ms_  = 0;
  return true;
}

void ObdTransportBle::write(const char* s) {
  if (s == nullptr) return;
  if (write_ == nullptr || !conn_) return;
  // ★ 无响应写（该特征 props=0x0C 支持）：有响应写会多一次往返，而 K 线本来就慢。
  //   ELM327 的指令都很短（"010C\r"），整串一次写出去即可。
  write_->writeValue(s, 0, false);
}

void ObdTransportBle::write(char c) {
  if (write_ == nullptr || !conn_) return;
  write_->writeValue((const uint8_t*)&c, 1, false);
}

// ---------------------------------------------------------------------------
//  射频仲裁：**执行点**（策略在 `lib/dashcore/radio_arbiter.h`，由 native 用例钉住）
// ---------------------------------------------------------------------------
void ObdTransportBle::setCoexPreference(bool ble_first) {
#if defined(ARDUINO)
  const esp_err_t e = esp_coex_preference_set(ble_first ? ESP_COEX_PREFER_BT
                                                        : ESP_COEX_PREFER_BALANCE);
#else
  const int e = 0;
#endif
  if (ble_first) coex_bt_err_ = (uint32_t)e; else coex_bal_err_ = (uint32_t)e;
  // ★ 只在**过渡**时打这一行（不是周期量）：抢/还各一行，
  //   配合 1 Hz 体检行里的 `windows/capped` 就能看出"抢了几次、被上限掐了几次"。
  dash_logf("obd-ble: 射频优先权 -> %s (esp_coex=%d)\n",
            ble_first ? "BT(给建连让路,<=8s)" : "BALANCE(还给链路)",
            (int)e);
}

void ObdTransportBle::applyRadioArbitration(uint32_t now_ms) {
  const bool hold = arb_.update(now_ms, wantsRadio(), ready());
  if (hold == coex_hold_) return;      // ★ 过渡才调 IDF；稳态一圈都不碰它
  coex_hold_ = hold;
  setCoexPreference(hold);
}

// ---------------------------------------------------------------------------
// 主循环状态机（非阻塞）
// ---------------------------------------------------------------------------
void ObdTransportBle::tick(uint32_t now_ms) {
  if (!started_) return;
  if (boot_ms_ == 0) boot_ms_ = now_ms;

  // ★★ 射频仲裁必须放在**最前面**，而且要在下面那个 `ready()` 早退**之前**：
  //   "连上 + grace 到 ⇒ 把射频还给链路"这件事正好发生在 ready 之后，
  //   放到早退后面就永远不执行（那就等于**连上之后还一直占着**）。
  //   它是状态机（每圈推进：占用有 8s 上限、让出有 30s 冷却），所以每圈都要调。
  applyRadioArbitration(now_ms);

  if (ready()) { backoff_ms_ = 0; return; }        // 好了，什么都不做

  // 退避：别把射频时间片全占了（ESP-NOW 那条链路还要吃饭）
  if (backoff_ms_ == 0) backoff_ms_ = 500;
  if ((uint32_t)(now_ms - last_try_ms_) < backoff_ms_) return;
  last_try_ms_ = now_ms;

  if (conn_ && !ready()) {                          // 连上但特征没齐 ⇒ 断了重来
    if (client_) client_->disconnect();
    onDisconnected();
  }

  if (want_peer_ && !conn_) {                       // 有地址了 ⇒ 连
    // ★★ 2026-09-27 车上实测（这条最花时间，写清楚）：**扫描看得到、连接却失败**，
    //   而且连 `onConnectFail` 都不打 ⇒ 连接请求**压根没发出去**。
    //   同一时刻用笔记本（bleak）连同一个头：**一连连妥**（FFF0/FFF1/FFF2 全读到）
    //   ⇒ 头没问题、地址类型没问题、也不是"被占着"。问题在板子这一侧的**时序**：
    //   `scan_->stop()` 之后控制器还在收尾，此时发起连接会被控制器直接拒掉
    //   （而我们看不到任何回调）。修法：**停扫之后真的等够**再连。
    //
    // ★ 计数（`cs_attempts` / `cs_calls`）是**为了不依赖日志**：
    //   日志环有每秒预算、可能把这种低频行丢掉；而这两个数直接进状态行，
    //   一眼就能分清"没进这个分支"与"进了但 connect 没返回"。
    if (scan_) {
      if (scan_->isScanning()) scan_->stop();
    }
    ++cs_attempts;
    // ★★ 这里**不要**再放"停扫后等 N ms"的闸门 —— 2026-09-27 车上踩到：
    //   函数开头（退避那一段）已经做过 `last_try_ms_ = now_ms`，
    //   此处的 `now_ms - last_try_ms_` **恒为 0**，于是 `< 600` 永远成立、
    //   每次都 return ⇒ **connect() 一次都发不出去**。
    //   现象极具误导性：state 停在 `connecting`、一个回调都没有，
    //   看起来像"对端不理我们"。真正的判据是计数器 `cs=a/b`：
    //   `a` 在涨而 `b` 恒为 0 ⇒ 卡在这儿，不是链路问题。
    //   ★ 节奏由函数开头那个**退避闸门**负责，这里不必再来一道。
    ++cs_calls;
    if (connectNow()) { ++connects_; backoff_ms_ = 0; return; }
    backoff_ms_ = (backoff_ms_ < 8000u) ? (backoff_ms_ * 2u) : 8000u;
    return;
  }

  // 没地址 ⇒ 扫一轮（非阻塞；扫到就由 onDiscovered 停下并记地址）
  if (scan_ && !scan_->isScanning()) {
    scan_->start(2000, false, false);
    backoff_ms_ = (backoff_ms_ < 8000u) ? (backoff_ms_ * 2u) : 8000u;
  }
}

bool ObdTransportBle::connectNow() {
  if (client_ == nullptr || !peer_valid_) return false;
  // ★★ 每一步都留痕（2026-09-27 车上排查）：现象是"`state=connecting` 卡住、
  //   一个回调都不来"，必须分清是
  //     (a) 压根没进这个函数、(b) `connect()` 阻塞住没返回、
  //     (c) `connect()` 返回 false 但库里不回调、(d) 连上了但特征没拿到。
  //   ⇒ 分别对应下面四条日志；日志只在这里打（进函数一次），不刷屏。
  dash_logf("obd-ble: → connectNow begin (peer=%s type=%u)\n", peer_, (unsigned)peer_addr_.getType());
  // ★★ `NimBLEClient::connect()` 里有**三条会立刻返回失败、且不回调 onConnectFail**
  //   的前置检查（见 `NimBLEClient.cpp` 开头）—— 这正是"卡在 connecting、一个回调
  //   都没有"的形状：
  //     · `!NimBLEDevice::m_synced`        → 主机还没和控制器同步
  //     · `m_connStatus != DISCONNECTED`   → **上一次连接的状态没清干净**（最像这个：
  //       失败路径若没把状态复位，之后每次调用都会当场被拒，永远自愈不了）
  //     · `address.isNull()`               → 地址无效
  //   把状态打出来，一眼就能归因。
  dash_logf("obd-ble:   pre: isConnected=%d connHandle=0x%04X\n",
            (int)client_->isConnected(), (unsigned)client_->getConnInfo().getConnHandle());
  // ★ 用**存下来的地址对象**（类型正确），别拿字符串重建 —— 见 onDiscovered 里的实测教训。
  const bool ok = client_->connect(peer_addr_);
  dash_logf("obd-ble: ← client->connect() = %d  isConnected=%d\n",
            (int)ok, (int)client_->isConnected());
  if (!ok) return false;

  NimBLERemoteService* svc = client_->getService(NimBLEUUID(kServiceUuid));
  if (svc == nullptr) return false;
  NimBLERemoteCharacteristic* n = svc->getCharacteristic(NimBLEUUID(kNotifyUuid));
  NimBLERemoteCharacteristic* w = svc->getCharacteristic(NimBLEUUID(kWriteUuid));
  if (n == nullptr || w == nullptr) return false;

  // 订阅通知：回调只往环里塞字节（分片到达由 ObdSource 那边按 \r 切行处理）
  if (!n->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
        if (ObdTransportBle::s_self_) ObdTransportBle::s_self_->pushBytes(data, len);
      })) {
    return false;
  }
  notify_ = n;
  write_  = w;
  return true;
}

#endif  // ARDUINO && OBD_BLE

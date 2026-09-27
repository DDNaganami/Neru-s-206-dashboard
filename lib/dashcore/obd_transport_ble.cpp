#include "obd_transport_ble.h"

#if defined(ARDUINO) && defined(OBD_BLE)

#include "dash_log.h"
// ★ 射频共存那两行要用 IDF 的 API。**头文件叫 `esp_coexist.h`**（不是 `esp_coex.h`）
//   —— 实测写错名字直接 `fatal error: esp_coex.h: No such file or directory`。
//   真实路径：`framework-arduinoespressif32-libs/esp32/include/esp_coex/include/esp_coexist.h`
//   （函数 `esp_coex_preference_set(esp_coex_prefer_t)`、枚举 `ESP_COEX_PREFER_BALANCE`）。
#if defined(ARDUINO)
#include <esp_coexist.h>
// ★ 2026-09-28：`connect()` 那行要把"内部自由堆"和错误码打在**同一行** ——
//   判"到底是不是内存"必须两个数一起看。头文件是 `esp_heap_caps.h`
//   （与 `esp_coexist.h` 同一个 `include/` 根，不用改 platformio.ini 的 -I）。
#include <esp_heap_caps.h>
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
  if (d == nullptr) return;
  if (!d->haveServiceUUID()) return;
  if (!d->isAdvertisingService(NimBLEUUID(kServiceUuid))) return;

  // ★★★ 2026-09-28：**已经就绪时直接忽略**（原来这里挡的是 `want_peer_`，那个语义不对）。
  //
  //   原来的写法是 `if (want_peer_) return;` —— 本意是"地址已经拿到了，别重复处理"。
  //   但 `want_peer_` 的含义其实是"**下一拍去连**"，它在**成功连上之前永远不会被清掉**
  //   ⇒ 于是这条早退等价于"**一旦扫到过就再也不更新地址**"：
  //     · 重扫那条路（`kRescanAfterFails`）把 `want_peer_` 清成 false 才能进来一次，
  //       命中之后又被置 true ⇒ 之后**再不刷新**；
  //     · 而真正该早退的条件是"**已经连上/就绪了**"（那才是不需要再扫的状态）。
  //   ⇒ 改成判 `ready()`。连上之后不再反复重建地址对象（省 CPU），
  //     而"没连上"期间每次扫描命中都会把**地址与地址类型**刷新成最新的。
  if (ready()) return;

  // ★★ 存**整个地址对象**（含地址类型）。实测教训：`aa:bb:cc:12:22:33` 是
  //   随机静态地址，若存成字符串再按 `BLE_ADDR_PUBLIC` 重建 ⇒ 连接永远失败，
  //   而日志只有 `conn=0`（看起来像"设备不在/被手机占着"），极易误判。
  peer_addr_  = d->getAddress();
  peer_valid_ = true;
  peer_rssi_  = d->getRSSI();   // ★ 2026-09-28：记下扫描那一刻的信号强度（判"弱链路"用）
  scan_tries_ = 0;              // ★ 2026-09-28：扫到了 ⇒ 扫描上限的计数复位
  last_slow_scan_ms_ = 0;
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

  // ---------------------------------------------------------------------------
  // ★★★ 2026-09-28：**射频抑制**（诊断开关，串口 `o` 切换）
  //
  //   为什么需要它：BLE 连不上时这段状态机是**每 ~600ms 撞一次、每次占射频最长 8s**
  //   的节奏（`cs` 计数一路涨、`win/cap` 也在涨）⇒ 射频几乎一直挂在 BT 这边，
  //   而 ESP-NOW 链路与**面板弹跳缓冲的填充**都要吃饭。副板出现过
  //   "横纹 + 图案上移"，正是带宽被抢的症状。
  //   ⇒ 没有这个开关时，要判"横纹是不是射频造成的"只能**重新刷一版关掉 BLE 的固件**，
  //     而刷机要几十秒、还会打断现场观察，**判据与变量一起动了也分不清**。
  //     有了它：敲一个字符就能把射频占用的**唯一变量**开/关，屏前直接 A/B。
  //
  //   ★ 放在 `ready()` 早退**之后**、一切射频动作**之前**：已连上时不受影响
  //     （连上了就继续用），只在"还在尝试"这条路上生效。
  //   ★ 仲裁器仍要每圈推进（上面那行），否则占用的窗口不会按超时释放。
  if (inhibited_) {
    if (client_ && client_->isConnected()) client_->disconnect();
    backoff_ms_ = 0;
    return;
  }

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
      if (scan_->isScanning()) {
        scan_->stop();
        // ★★★ 2026-09-28：**记下停扫的那一刻**，供下面那道"停扫后等够"的闸门用。
        //   `onDiscovered()` 里也会停扫（找到就停），它不经过这里 ⇒ 那种情况下
        //   地址到手已经有时间间隔了，闸门自然放行（见下面的判据）。
        scan_stop_ms_ = now_ms;
      }
    }
    // ★★★ 2026-09-28 修正：**把"停扫之后等够再连"这道闸门补回来，并且给它自己的时间戳**。
    //
    //   来龙去脉（这是本文件最容易读歪的一段，两次都栽在同一处）：
    //     · 2026-09-27：发现"停扫之后立刻连会被控制器直接拒掉"，于是加了一道等 600ms 的闸门；
    //     · 但那次用的是 `now_ms - last_try_ms_`，而 `last_try_ms_` **在函数开头刚被赋值**
    //       ⇒ 差值恒为 0 ⇒ 闸门永远成立 ⇒ **connect() 一次都发不出去**（那是另一个 bug）；
    //     · 修那个 bug 时把整道闸门**删掉了**，却没有换成"用自己的时间戳"的正确实现
    //       ⇒ 于是"停扫后立刻连"这个原始问题又回来了，而且这次表现为
    //         `E NimBLEClient: Connection failed; status=13`（BLE_HS_ETIMEOUT）、
    //         **毫秒级**返回、`onConnectFail` 不触发 —— 与"前置检查失败"的形状一模一样，
    //         所以很容易被误判成"内存不够/地址无效/主机没同步"。实测 2026-09-28：
    //         18~19ms 返回、`cs=27/27`（27 次全败）、同一时刻扫描是能扫到对端的。
    //   ⇒ 本闸门**只认 `scan_stop_ms_`**（上面刚停扫才置位），绝不碰 `last_try_ms_`：
    //       停扫那一刻起算，满 `kPostScanSettleMs` 才允许 connect。
    //   ★ 为什么 600ms 够：控制器停扫是"命令已受理 + 射频收尾"，量级是几十毫秒；
    //     600ms 是留了 10 倍余量（宁可慢 0.6 秒，也不要每次都被当场拒）。
    //   ★ 判据：改完之后若有 `onConnectFail` / 真超时（≈15s）出现，说明请求**真的发出去了**，
    //     那才是"时序"这一关过了；仍 `status=13` + 毫秒级返回 ⇒ 还没发出，回来再查。
    if (scan_stop_ms_ != 0u) {
      if ((uint32_t)(now_ms - scan_stop_ms_) < kPostScanSettleMs) return;   // 等够再来
      scan_stop_ms_ = 0u;      // 放行一次即可，之后按正常退避节奏走
    }
    ++cs_attempts;
    ++cs_calls;
    if (connectNow()) { ++connects_; backoff_ms_ = 0; return; }
    backoff_ms_ = (backoff_ms_ < kRetryBackoffMaxMs) ? (backoff_ms_ * 2u) : kRetryBackoffMaxMs;

    // ★★★ 2026-09-28：**连败若干次之后，把地址丢掉、重新扫**。
    //
    //   为什么必须这么做（这是"地址会不会过期"这个盲点的补丁）：
    //     `peer_addr_` 是**开机扫描那一刻**存下来的，之后**再也不刷新**
    //     （`onDiscovered()` 里有 `if (want_peer_) return;`，拿到就不再收新的）。
    //     于是——**诊断头换过、或它自己重启后换了随机静态地址**——板子就会拿着
    //     一个**过期地址**一直撞：对端根本不在那个地址上，控制器**当场拒绝**
    //     （实测形状：`E NimBLEClient: Connection failed; status=13`、**17ms** 返回、
    //      `onConnectFail` 不触发、`cs` 一路涨而 `connects=0`）。
    //     ⇒ 这个错误形状**与"地址类型不对"完全一样**，光看日志分不出来，
    //       所以两条都要堵：地址类型靠存整个 `NimBLEAddress` 保证（见 onDiscovered），
    //       **地址本身的新鲜度靠这里**。
    //   ★ 阈值取 5：正常车上（头就在那儿）永远走不到这一步 —— 因为连上就 `ready()` 早退；
    //     真走到这里说明"手里这个地址没用"，重扫一次的代价远小于继续盲撞。
    //   ★ 重扫会重新走 `onDiscovered()`，把地址与**地址类型**一起刷新，
    //     并复位 `scan_tries_`（扫描上限计数）。
    if (cs_attempts != 0u && (cs_attempts % kRescanAfterFails) == 0u) {
      dash_logf("obd-ble: 连败 %u 次 ⇒ 丢掉地址重扫(地址可能已过期:换头/对端重启)\n",
                (unsigned)cs_attempts);
      peer_valid_ = false;
      want_peer_  = false;
      peer_[0]    = '\0';
      scan_stop_ms_ = 0u;
      scan_tries_   = 0;      // 让重扫走"快速"节奏，别等到 5 分钟那一档
    }
    return;
  }

  // 没地址 ⇒ 扫一轮（非阻塞；扫到就由 onDiscovered 停下并记地址）
  // ★★ 2026-09-28：**扫描必须有上限**（当天那两例"主板发送面停摆"的可疑诱因）：
  //   台面上没有诊断头时，这个分支原来会**永远每 ~8 秒扫 2 秒**（占空比 ~25%），
  //   把射频时间从 ESP-NOW 的发送完成侧抠走 ⇒ 在途窗口填满、`done` 追不平 ⇒ 停摆。
  //   ⇒ 快速试 `kScanTriesFast` 轮之后退成 **每 `kScanSlowMs` 试一次**（车上插着诊断头
  //     时正常路径不受影响：扫到就 `onDiscovered()`，计数与慢速计时都被复位）。
  if (scan_ && !scan_->isScanning()) {
    const bool fast = (scan_tries_ < kScanTriesFast);
    if (fast) {
      ++scan_tries_;
      scan_->start(2000, false, false);
    } else if ((uint32_t)(now_ms - last_slow_scan_ms_) >= kScanSlowMs) {
      last_slow_scan_ms_ = now_ms;
      scan_->start(2000, false, false);
    }
    backoff_ms_ = (backoff_ms_ < kRetryBackoffMaxMs) ? (backoff_ms_ * 2u) : kRetryBackoffMaxMs;
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
  // ★★ 并且**直接量真实耗时**（2026-09-28）：日志行之间的"行距"**不能**当耗时用 ——
  //   日志环有每秒预算、这种低频行会被丢。这个数决定方向：
  //     · 接近 15000ms ⇒ 连接请求发出去了、对端不理（真超时，往对端/射频查）；
  //     · 毫秒级      ⇒ 库或控制器**当场拒绝**（前置检查失败），与射频无关。
  const uint32_t t0 = millis();
  const bool ok = client_->connect(peer_addr_);
  const uint32_t dt = (uint32_t)(millis() - t0);
  // ★★★ 2026-09-28：**把真实错误码打出来**（docs/BLE-OBD.md §12.4 的第一步）。
  //
  //   为什么必须打：`connect()` 只返回 bool，真实的 `rc` 被丢在里面；而库在
  //   快速路径（`taskWait` 的超时 0-tick 那条）返回时**从不设置** `taskData.m_flags`
  //   ⇒ 它随后补写的 `BLE_HS_ETIMEOUT(13)` 是**兜底值、不是控制器给的错误码**
  //   （见 §12.1 的源码推导）。所以 `status=13` 这条线索一直是死的，两条基于它的
  //   推断（"内存不够"、"地址类型不对"）都已作废。
  //   ⇒ 要拿真值只有两条路，这里走**第一条**：
  //     ① `client_->getLastError()` —— 库在 `error:` 标号处会 `m_lastErr = rc`，
  //        所以它比日志里的 `status` 可信（`connect()` 失败必经那里）；
  //     ② 改 `asyncConnect = true` + 回调驱动（结构性绕开快速路径）—— 还没做。
  //   ★ 判据（怎么读这个数）：
  //     · `0` 或仍是 13 ⇒ `getLastError()` 也没拿到，说明失败**根本没走到 error 标号**，
  //        那就只剩 ② 那条路（改异步 + 回调）。
  //     · 非 0 且不是 13 ⇒ **这就是真错误码**，按 NimBLE 错误表查（如
  //        `BLE_HS_ENOTSYNCED=8` / `BLE_HS_EREJECT=14` / `BLE_HS_EINVAL=3` /
  //        `BLE_HS_ETIMEOUT_HCI=17` 等），不要再去猜内存。
  //   ★ 同时把 `free heap` 打进来：判"到底是不是内存"需要它和错误码**同一行**，
  //     否则"OOM"与"协议拒绝"在日志上分不开。
  dash_logf("obd-ble: ← client->connect() = %d  isConnected=%d  耗时=%ums  rssi=%d"
            "  lastErr=%d  内部free=%uKB\n",
            (int)ok, (int)client_->isConnected(), (unsigned)dt, (int)peer_rssi_,
            (int)client_->getLastError(),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024u));
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

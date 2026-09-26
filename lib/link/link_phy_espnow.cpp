// 双板链路协议 v1 —— ESP-NOW 的 LinkPhy 实现（无线那一档）。
//
// 三条"非阻塞"到底靠什么成立（全部写在代码里，别只看文件头）：
//   · write()      → 只把字节拷进 mTxBuf，**碰都不碰射频** ⇒ 绝不阻塞。
//   · pumpTx()     → 只把环里**够一整帧**的那些字节交给 `esp_now_send()`；
//                    那个函数只把包拷进驱动待发队列就返回（**不等回调**），
//                    单次调用最多 kPumpPackets 个包（§1.3 的"有上界"）。
//   · availableForWrite() → min(环剩余, 在途余量) ⇒ "写这么多不阻塞"。
//   · read()       → 只从 mRxBuf 取；mRxBuf 由 WiFi 任务的接收回调填
//                    ⇒ 主循环不碰射频锁、也不做任何格式化。
//
// ★★ 唯一一处诚实的保留：`esp_now_send()` 仍可能返回 `ESP_ERR_ESPNOW_NO_MEM`
//    （驱动待发队列满）。那时这一帧**整帧丢掉并计数**（`txSendFail`），
//    绝不自旋、绝不 delay —— 而"在途上限"（kEspNowMaxPending）就是为了让这件事
//    **尽量不发生**：在途数到顶时 `availableForWrite()` 报 0，上层（LinkTx::pump）
//    自然就少发，不会把帧堆到驱动的队列上去。
#include "link_phy_espnow.h"

#if LINK_PHY_ESP_NOW

// ★ 只在 S3 上编：ESP-NOW 是 WiFi 那半边射频的东西，而"两块板用无线对打"
//   这件事的前提本来就是这两块 S3（契约 §0 的双板架构）。经典 ESP32 上
//   这一档没有意义（它连 `LINK_PHY_UART` 都不带，见 platformio.ini）。
#if defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "link_phy_espnow 只在 ESP32-S3 上编(双板架构的两块板就是 S3)。请去掉该 env 的 -DLINK_PHY_ESP_NOW。"
#endif

#include <WiFi.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "dash_log.h"   // 只为了开机/学习那几行——**不**在回调里调它（见文件头 ②）

// ★★ 第二道闸门（与头文件里那条**同一条判据**、同一个理由）：
//   头文件那条只在"有人 include 这个头"时才生效，而 `.cpp` 是"这份固件真的有
//   无线 PHY"的**唯一证据**（LDF 只把被引用到的库源码编进来）⇒ 两条都留，
//   与 `link_phy_uart.cpp` 的做法一致（那边也是 .cpp 里判）。
#if defined(DASH_LOG_UART0) && (DASH_LOG_UART0 != 0)
#error "link_phy_espnow.cpp: 无线链路 PHY 进了固件，而日志还写着 UART0(DASH_LOG_UART0!=0)。见头文件里那道闸门的说明。"
#endif

namespace dashlink {

namespace {

// ★★ 活动实例指针（2026-09-27 **上板实测抓出来的一个真 bug 的修法**）
//
// ESP-NOW 的回调只能注册**全局函数指针**（没有"用户数据"参数），所以回调里必须知道
// "该把事件交给哪个对象"。原来这里写的是**文件级实例** `LinkPhyEspNow g_espnow;`，
// 而 `src/main.cpp` 用的是它自己那个 `static LinkPhyEspNow g_link_phy;`
// ⇒ **回调转发到了另一个对象上**：收到的字节永远进不了 main 读的那个环、
//   "学到对端"那行永远不打印、发送完成回调减的是**别人**的在途计数。
// 症状（2026-09-27 上板实测，两块板都插着、屏都在跑）：
//     · 两边都 `rx bytes=0 frames=0`；从板 `sim tick_age=… seen=0`；
//     · 两边都**没有** `espnow: peer=… learned`；
//     · 而主板 `link: tx=` **一直在涨**（写进了环、却没真发出去）。
// ⇒ 修法：**由 `begin()` 把 `this` 绑进来**（"一份固件只有一个链路 PHY"这条设计
//   意图不变 —— 这里仍然只保存**一个**指针）。绑定之后两个角色/两种写法都对：
//   main 的 `g_link_phy` 一 `begin()`，回调就知道该找谁。
LinkPhyEspNow* g_active = nullptr;

// 这一包像不像"测量信封"的载体帧：v1 帧头 + `TYPE=DATA` + `LEN=9` + 载荷开头 `DSM1`。
// ★ 只做几个字节比较：**回调里不许做重活**（不算 CRC、不格式化、不打日志）。
//   真正的判据仍在 `MeasReceiver::noteArrival()` 里（magic + 版本），这里只是"要不要叫它"。
bool looksLikeMeasCarrier(const uint8_t* d, int len) {
  if (d == nullptr || len != (int)(kOverhead + 9u)) return false;
  if (d[kOffSync] != kSync) return false;
  if (d[kOffType] != (uint8_t)MsgType::Data) return false;
  if (d[kOffLen] != 9u) return false;
  const uint8_t* p = d + kOffPayload;
  return p[0] == 'D' && p[1] == 'S' && p[2] == 'M' && p[3] == '1';
}

// 可读的 MAC 文本（`aa:bb:cc:dd:ee:ff`）。★ 静态缓冲：只在"刚学到/要打日志"
// 那一刻有效，返回的指针**别存**（本类是单线程主循环用的，这一条够用）。
const char* kHex = "0123456789abcdef";
char g_peerText[18] = {0};
const char* macText(const uint8_t* mac) {
  if (mac == nullptr) return "(none)";
  uint8_t o = 0;
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) {
    g_peerText[o++] = kHex[(mac[i] >> 4) & 0x0Fu];
    g_peerText[o++] = kHex[mac[i] & 0x0Fu];
    if (i + 1u < kEspNowMacLen) g_peerText[o++] = ':';
  }
  g_peerText[o] = '\0';
  return g_peerText;
}

bool macIsBroadcast(const uint8_t* mac) {
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) {
    if (mac[i] != 0xFFu) return false;
  }
  return true;
}

bool macEqual(const uint8_t* a, const uint8_t* b) {
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

// NVS 写入的**上界**（见 .h 里"学到后存 NVS"那条）：正常只会写 1 次
// （开机从 NVS 读、第一次学到就写一次，之后 mac 不变就不再写）。
// 这条上限是防"对端反复重启/被杂散设备带着改 mac"把 flash 写坏。
const uint8_t kMaxNvsSaves = 4u;

// ★★ 落盘之前要先看到**这么多**帧"形态合法的 v1 帧"（2026-09-27 收紧）：
//   为什么不是"学到就写"：单帧形态合法**不足以**证明"这就是我们的对端"，
//   而写进 NVS 的代价是"重启之后仍然单播给错的 MAC ⇒ 两端静默 rx=0"。
//   取 8：对端在 50 Hz 的 TICK 下 8 帧只要 ~160 ms ⇒ 对**真对端**几乎是零延迟；
//   而对**杂散设备**来说，"连送 8 帧结构正确的链路帧"已经不太可能是巧合。
const uint32_t kPeerFramesBeforeNvs = 8u;

// ★ 那一行计数器的**周期**（2000 ms）：与 `link:` 那行（每秒）同一个量级，
//   但**故意慢一倍** —— 它的用途是"看趋势/看从哪一刻开始不动"，不是逐秒对账。
const uint32_t kPhyLogPeriodMs = 2000u;

}  // namespace

// ★ 测量帧的到达钩子（定义）。缺省 nullptr ⇒ 收帧路径与以前**逐字节相同**。
//   ★ 必须写在**匿名 namespace 之外**：它是一个类静态成员，写在里面编译器会拒
//     （definition is not in namespace enclosing ...）。
LinkPhyEspNow::MeasSinkFn LinkPhyEspNow::g_meas_sink = nullptr;

// ============================================================
// 回调（★ WiFi 任务上下文：只拷字节 + 计数，别的什么都不做）
// ============================================================

void LinkPhyEspNow::onRecvStatic(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  // ★ 签名与 IDF 的 `esp_now_recv_cb_t` **逐字一致**（这一版 arduino-esp32 3.3.9
  //   的 IDF 5.5 用的是带 `esp_now_recv_info_t` 的那一版；旧版是
  //   `(const uint8_t* mac, const uint8_t* data, int len)`）。
  //   这里只做一次短跳转，不复制载荷。
  const uint8_t* src = (info != nullptr) ? info->src_addr : nullptr;
  if (g_active != nullptr) g_active->onRecv(src, data, len);
}

// 一个包"像不像一条 v1 链路帧"——**只做形态判据：SYNC + 版本 + LEN 合法 + 长度自洽**。
// ★ 为什么**不在这里**算 CRC：CRC 要在 WiFi 任务上下文里遍历整帧，而接收回调的纪律是
//   "只拷字节 + 计数"（见文件头 ②）。形态判据已经足够回答"这是不是我们的对端"——
//   CRC 不过的帧到不了 `LinkRx`（那才是算 CRC 的地方），而这里只决定"要不要学这个 MAC"。
// ★ 长度自洽 = `len == 7 + LEN`（§2 的"帧长 = 7 + 载荷"），这一条把"恰好凑巧前 4 个字节
//   对上"的随机包挡掉；再加上 LEN 落在 4..16，误学的概率已经足够低。
bool looksLikeV1Frame(const uint8_t* d, int len) {
  if (d == nullptr || len < (int)kOverhead) return false;
  if (d[kOffSync] != kSync) return false;
  const uint8_t ver = d[kOffVer];
  if ((uint8_t)(ver >> 4) != kVerMajor) return false;        // 主版本必须对（§2）
  const uint8_t plen = d[kOffLen];
  if (plen < kLenMin || plen > kLenMax) return false;
  return (int)frameBytesForLen(plen) == len;
}

void LinkPhyEspNow::onRecv(const uint8_t* src, const uint8_t* data, int len) {
  if (data == nullptr || len <= 0) return;

  // ---- ① 学对端 MAC（自配置寻址的核心）----
  // ★★ 判据收紧（2026-09-27，车主/Grok 复核）：**只从一帧"形态合法的 v1 帧"学**，
  //    而不是"收到任何一帧就学"。为什么这条必须收紧：
  //      · 同信道上可能有**别人的** ESP-NOW 设备（台面上就有：抓帧盒、别的实验板）；
  //      · 老写法一旦被这种包先撞上，就会把**错的** MAC 记下来（而且写进 NVS），
  //        之后单播发给它 ⇒ **两端都静默 `rx=0`**，还查不出原因（重启也从 NVS 读回来）。
  //    ⇒ 现在：形态不对的包**既不学、也不入环**（记 `rxForeign` 一份，好让"有杂散设备"
  //      这件事在日志里看得见）。
  const bool plausible = looksLikeV1Frame(data, len);
  if (src != nullptr && !mPeerKnown && !macIsBroadcast(src)) {
    if (plausible) {
      setPeer(src);
    } else {
      ++mRxForeign;
      return;
    }
  }
  // ② 已经在单播了：只收对端的（别人的包不算链路质量 —— 记 rxForeign）
  if (src != nullptr && mPeerKnown && !macEqual(src, mPeer)) {
    ++mRxForeign;
    return;
  }
  // ★ 静态配对档（`-DLINK_ESPNOW_PEER_MAC`）里 `mPeerKnown` 开机就是 true ⇒ 上面那条
  //   会把非对端的包统统挡掉；形态不对的包也挡掉（我们只把**我们的帧**放进环）。
  if (!plausible) {
    ++mRxForeign;
    return;
  }
  if (src != nullptr) ++mPeerFrames;    // ★ "这个对端确实带来了可用的帧"（NVS 只在这儿之后写）

  // ---- ②b ★★ 测量帧的**到达钩子**（2026-09-27 深夜，本单第二步）----
  //   在**收到包这一刻**（WiFi 任务上下文）把信封交给钩子去打时间戳，然后
  //   **直接返回、不进 RX 环**。理由见 `link_phy_espnow.h` 里 `setMeasSink` 那段：
  //   主循环里打时间戳会把"收端主循环被抢占 ~100ms"混进到达间隔里
  //   （实测 `gap_max` 113ms / p99 42ms，而同一轮 `wire_gap_max` 只有 10ms）。
  //   ★ 放在这里（学完对端、判过形态之后）而不是函数开头：这样"学到对端"这件事
  //     不依赖有没有注册钩子；而 `plausible` 已经保证它是一条形态合法的 v1 帧。
  if (g_meas_sink != nullptr && looksLikeMeasCarrier(data, len)) {
    if (g_meas_sink(data + kOffPayload, (uint8_t)9u, (uint32_t)millis())) {
      ++mRxMeasConsumed;
      return;                            // 已被消费：不进环、也不再算 rx_frames
    }
  }

  // ---- ③ 整包进入环（**整包进出**：环里放不下这一包就不放，绝不分两段） ----
  //   ★ 为什么整包进出：`read()` 是逐字节给出去的（与 UART 那一档同形），
  //     半个包进来会让上层解出一个永远等不到尾巴的帧 —— 那比丢包更糟
  //     （LinkRx 会一直攒着它）。环按字节环形，但**包不跨环的接缝**。
  const uint16_t n = (uint16_t)len;
  if (n > kRxRingBytes) {              // 单包比环还大：不可能（链路帧 ≤71 B）
    mRxOverflow += n;
    return;
  }
  // ★★ 真正的可用空间 = `(tail - 1 - head) mod N`（留一格区分"空/满"），**允许环绕**。
  //
  // 为什么必须这么算 —— 2026-09-27 上板实测抓到的**永久死锁**（本文件最贵的一个 bug）：
  //   老写法是"从 head 到 tailNext 这一段连续空位"，而**环空**（head == tail == H）时
  //   它算出来是 `N - H`：
  //     · H 因为包长累积会落在任意值上；一旦 H 落进 **241..255**，
  //       `N - H` 就小于最短的帧（TICK = 13 B）⇒ 任何一个包都放不下；
  //     · 而 `mRxHead` **只在放得下时才前进** ⇒ 它再也回不到 0 去绕环 ⇒ 卡死。
  //   实测症状与之一一对应：`rx_frames/rx_bytes` 冻住、`overflow` 无界增长
  //   （每次 +2500 B 左右）、`gap_rx` 单调涨、`tx`/`tx_fail` 完全正常、两块板都一样。
  //   ⇒ 这不是射频问题，是**环形缓冲的空位算法**问题。
  //
  // ★ 环绕是安全的，不要再用"包不许跨接缝"去挡它：下面那个逐字节拷贝本来就用
  //   `% kRxRingBytes`；而且 `mRxHead` 是**整包拷完之后**才前进的（第 190~191 行），
  //   读侧永远不可能看到半个包。老注释里"半个包进来会让上层解出一个永远等不到尾巴
  //   的帧"担心的是**拷贝一半就暴露 head**，那件事在这个写法下不会发生。
  const uint16_t freeBytes =
      (uint16_t)((mRxTail + kRxRingBytes - 1u - mRxHead) % kRxRingBytes);
  if (n > freeBytes) {
    mRxOverflow += n;
    return;
  }
  for (uint16_t i = 0; i < n; ++i) mRxBuf[(mRxHead + i) % kRxRingBytes] = data[i];
  mRxHead = (uint16_t)((mRxHead + n) % kRxRingBytes);
  mRxTotal += n;
  ++mRxFrames;
  // ★ "距上一次收到包多久"这条判据的锚点（主循环那行汇总要报它）：
  //   为什么需要它：上板实测"两个方向同时停住"时，**光看 rx_frames 冻住**分不清
  //   "本来就没有包"与"射频已经静默"—— 有了这个数就一眼可见。
  mLastRxMs = millis();
}

void LinkPhyEspNow::onSendDoneStatic(const esp_now_send_info_t* tx_info,
                                     esp_now_send_status_t status) {
  // ★ 同样是 IDF 的当前签名（`esp_now_send_cb_t`）—— 见 esp_now.h 里那个 typedef。
  (void)tx_info;
  if (g_active != nullptr) g_active->onSendDone((uint8_t)status);
}

void LinkPhyEspNow::onSendDone(uint8_t status) {
  // ★ 这里只做计数：**不格式化、不打日志、不碰环**（WiFi 任务上下文，
  //   而"日志同步写"这条路本项目今天刚栽过一次，见 link_phy_uart.h 的实测那条）。
  if (status == 0u) ++mTxDone;          // ESP_NOW_SEND_SUCCESS == 0
  else ++mTxStatusFail;
  if (mPending > 0u) --mPending;
}

// ============================================================
// 初始化
// ============================================================

void LinkPhyEspNow::begin(bool loopback) {
  // ★★ **第一件事：把回调绑到"我"身上**（见 g_active 那段 —— 这是 2026-09-27 上板
  //   实测抓出来的 bug 的修法：回调原来转发给一个**跟 main 无关的**文件级实例）。
  g_active = this;
  mLastSendErr = ESP_OK;
  mPhyLogAtMs = millis();
  mSendFailLogged = false;
  mFirstSendFailAtMs = 0;
  mFirstSendFailErr = 0;
  mLastRxMs = 0;
  mOnline = false;
  mPeerKnown = false;
  mPending = 0;
  mTxHead = mTxCount = 0;
  mRxHead = mRxTail = 0;
  mTxTotal = mTxFrames = mTxOverflow = mTxDropFrames = mTxSendFail = 0;
  mRxTotal = mRxOverflow = mRxFrames = mRxForeign = 0;
  mTxDone = mTxStatusFail = 0;
  mPeerLearned = mNvsLoads = mNvsSaves = 0;
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) mPeer[i] = 0;

  if (loopback) {
    // 无线没有"本机回环"这一说 —— 说出来，别让人以为它生效了。
    dash_logf("espnow: loopback=true 被忽略（无线没有本机回环这一档）\n");
  }

  // ---- ① 射频：STA 模式、不连 AP、信道固定 ----
  //   ★ 为什么是 STA 而不是 AP：ESP-NOW 两种都行，但 STA 不会起 SoftAP
  //     （不额外占资源、也不会让别人连上来）；`ifidx` 也就固定是 WIFI_IF_STA。
  WiFi.persistent(false);            // 不要把 Wi-Fi 设置写 NVS（那是另一套 flash 写）
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);      // ★ 我们**根本不连任何 AP** ⇒ 别让协议栈去扫描/重连
  WiFi.disconnect();                 // 不连任何 AP（我们只用 ESP-NOW 那一层）
  // ★★ 省电模式：**显式关掉**（2026-09-27 追加）。
  //   为什么（这条不是"省电不好"，是判据不允许）：IDF 默认是 modem power-save，
  //   射频在两次收包之间会打盹、靠 DTIM 节拍唤醒 —— 那个唤醒抖动正是**几十毫秒**量级，
  //   而这条链路的判据是 **p99 抖动 < 20 ms**（一个 TICK 周期，见 link_meas.h）。
  //   ⇒ 开着省电时我们会把"节拍误差"记成"链路抖动"，然后白折腾一轮排查。
  //   ★ 代价：射频常开 ⇒ 功耗高一些（车上不缺电；代价与数字见 ARCHITECTURE §8.3.7）。
  const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
  if (ps_err != ESP_OK) {
    dash_logf("espnow: esp_wifi_set_ps(WIFI_PS_NONE) 失败 err=%d —— 抖动可能被省电节拍污染\n",
              (int)ps_err);
  }
  // ★★ 信道固定：ESP-NOW **不跨信道**，两端必须同一个；配错的症状是
  //   "两端都打 rx=0"，不会有任何编译期信号 ⇒ 所以这里显式设、日志里显式打。
  const esp_err_t chan_err = esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE);
  if (chan_err != ESP_OK) {
    dash_logf("espnow: esp_wifi_set_channel(%u) 失败 err=%d —— 链路不会通\n",
              (unsigned)kChannel, (int)chan_err);
    return;
  }

  // ---- ② ESP-NOW 本体 ----
  const esp_err_t init_err = esp_now_init();
  if (init_err != ESP_OK) {
    dash_logf("espnow: esp_now_init 失败 err=%d —— 链路不会通\n", (int)init_err);
    return;
  }
  esp_now_register_recv_cb(&LinkPhyEspNow::onRecvStatic);
  esp_now_register_send_cb(&LinkPhyEspNow::onSendDoneStatic);

  // ---- ③ 广播 peer：自配置寻址的**第一跳**（还不知道对端 MAC 时靠它） ----
  //   ★ 静态配对（`-DLINK_ESPNOW_PEER_MAC=…`）时**也要加**：那一档的"开机直接单播"
  //     靠下面的 ④ 分支去加单播 peer，而广播 peer 留着无害
  //     （它只在"还没学到对端"时才会被用到；见 link_phy_espnow.h 的第三条）。
  const bool bcast_ok = addPeerBroadcast();
  if (!bcast_ok) {
    dash_logf("espnow: 添加广播 peer 失败 —— 开机那几秒发不出 HELLO\n");
  }
  // ★★ 广播 peer **到底在不在表里**（2026-09-27 追加的自证）：`addPeerBroadcast()`
  //   的返回值**不足以下结论**（重复添加本来就会返回 `ESP_ERR_ESPNOW_EXIST`，我们把它
  //   当成功了）⇒ 这里用 `esp_now_is_peer_exist()` 直接问驱动，并把 peer 总数一起打。
  //   上板时"一帧都发不出去"而这个数能一眼看出是不是"表里根本没那个 peer"。
  {
    const bool bcast_in = esp_now_is_peer_exist(kEspNowBroadcast);
    esp_now_peer_num_t pnum{};
    esp_now_get_peer_num(&pnum);
    dash_logf("espnow: bcast_peer=%s peers=%d (add=%s)\n", bcast_in ? "yes" : "NO",
              (int)pnum.total_num, bcast_ok ? "ok" : "fail");
  }

  // ★★ 本机 STA MAC（2026-09-27 追加）：**开机就打**，别只在"学到对端"那一行之后
  //   才知道对端是谁。为什么值得单独一行：
  //     · 静态配对（`-DLINK_ESPNOW_PEER_MAC=…`，见 cfg 头文件）要的就是**这两块板
  //       各自的 MAC** —— 没有这一行，两只板子的 MAC 都拿不到，静态配对根本配不出来；
  //     · 台面上同时开着好几个 ESP-NOW 设备时，"到底谁跟谁"要靠这一行对账；
  //     · 它与"学到对端"那一行（`espnow: peer=… learned`）**成对**看：
  //       本机 MAC 是 A 板打的、peer 是 B 板的 MAC —— 拿 A 的这一行去对 B 的那一行，
  //       两边应当**互为对方的 peer**（这条判据不需要任何额外工具）。
  //   ★ 走 `dash_logf()`（日志环），**不**在射频初始化路径里直接写串口。
  {
    uint8_t own[kEspNowMacLen] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, own) == ESP_OK) {
      dash_logf("espnow: sta_mac=%s ch=%u rxring=%u txring=%u ps=off\n", macText(own),
                (unsigned)kChannel, (unsigned)kRxRingBytes, (unsigned)kTxRingBytes);
    } else {
      dash_logf("espnow: sta_mac 读不出来(esp_wifi_get_mac 失败) ch=%u rxring=%u txring=%u\n",
                (unsigned)kChannel, (unsigned)kRxRingBytes, (unsigned)kTxRingBytes);
    }
  }

  // ---- ④ 对端地址从哪来（按优先级）----
  //   ① **编译期静态配对**（`-DLINK_ESPNOW_PEER_MAC`，见 cfg 头文件）：
  //      跳过发现、开机直接单播 —— 表仓里只有两块板时这是最稳的一档
  //      （发现广播的唯一风险是"同信道上还有别人的 ESP-NOW ⇒ 学到错的邻居"）。
  //   ② **NVS 里上一次学到的**：读到就单播，不必再等一轮广播 HELLO。
  //   ③ 都没有 ⇒ 靠广播 HELLO 发现（见 .h 文件头第三条）。
#if defined(LINK_ESPNOW_PEER_MAC)
  {
    const uint8_t mac[kEspNowMacLen] = {LINK_ESPNOW_PEER_MAC};
    for (uint8_t i = 0; i < kEspNowMacLen; ++i) mPeer[i] = mac[i];
    mPeerKnown = true;
    addPeerUnicast();
    dash_logf("espnow: peer=%s 来自编译期 -DLINK_ESPNOW_PEER_MAC（跳过发现广播）\n",
              macText(mPeer));
  }
#else
  loadPeerFromNvs();
#endif

  mOnline = true;
}

bool LinkPhyEspNow::addPeerBroadcast() {
  esp_now_peer_info_t p{};
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) p.peer_addr[i] = 0xFFu;
  p.channel = kChannel;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  p.priv = nullptr;
  const esp_err_t e = esp_now_add_peer(&p);
  // ALREADY EXISTS 也算成功（IDF 的 esp_now_init 会预置广播 peer）
  return (e == ESP_OK) || (e == ESP_ERR_ESPNOW_EXIST);
}

bool LinkPhyEspNow::addPeerUnicast() {
  if (!mPeerKnown) return false;
  esp_now_peer_info_t p{};
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) p.peer_addr[i] = mPeer[i];
  p.channel = kChannel;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  p.priv = nullptr;
  const esp_err_t e = esp_now_add_peer(&p);
  return (e == ESP_OK) || (e == ESP_ERR_ESPNOW_EXIST);
}

// ============================================================
// 寻址：学到 → 记下 →（**确认对端真的在发我们的帧之后**）才存 NVS
// ============================================================

void LinkPhyEspNow::setPeer(const uint8_t* mac) {
  if (mac == nullptr) return;
  const bool changed = !mPeerKnown || !macEqual(mac, mPeer);
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) mPeer[i] = mac[i];
  mPeerKnown = true;
  if (!changed) return;
  ++mPeerLearned;
  addPeerUnicast();                  // 下一圈 `pumpTx()` 就单播
  // ★ 日志口径：`espnow: peer=xx:xx:… learned`（人一眼能抄下来、也一眼能看出
  //   "到底学没学到"）。这一行**只在主循环上下文出现** —— onRecv 那条学习路径跑在
  //   **WiFi 任务**里，而见 .h 文件头 ②：**回调里不打日志**（在 WiFi 任务里同步写
  //   USB-CDC 会把整个射频任务拖住，正是本项目栽过的那类事故）。
  //   ⇒ 这里只把"要打的那一行"记下来，由下一次 `pumpTx()`（主循环）打出去。
  mPendingLogPeer = changed && (mLogCount < 2u);
  // ★★ NVS **不在这里写**（2026-09-27 收紧，车主/Grok 复核）："学到"只是"收到了一个
  //    形态合法的 v1 帧"，而**那还不足以证明它就是我们的对端**（同信道上可能有别人的
  //    ESP-NOW 设备，形态也可能撞上）。写进 NVS 的后果很重：**重启之后仍然单播发给
  //    那个错 MAC** ⇒ 两端都静默 `rx=0`，而且查不出原因（下一单现场最可能踩的坑）。
  //    ⇒ 现在改成"**等这个对端真的送来若干帧之后**才写"（判据见 `flushPeerLog()`）。
}

void LinkPhyEspNow::flushPeerLog() {
  if (mPendingLogPeer) {
    mPendingLogPeer = false;
    ++mLogCount;
    dash_logf("espnow: peer=%s learned\n", macText(mPeer));
  }
  // ★★ "确认可用之后才落盘"：`kPeerFramesBeforeNvs` 帧形态合法的 v1 帧到了 ⇒ 这几乎
  //    不可能是杂散设备（它得连续送出结构正确的链路帧）⇒ 这时才写 NVS。
  //    ★ 只在主循环里调（`pumpTx()` 的第一行），所以 NVS 的读-改-写不会被两个上下文
  //      同时碰 —— 也不会在 WiFi 任务里阻塞射频。
  if (mPeerKnown && mNvsSaves == 0u && mPeerFrames >= kPeerFramesBeforeNvs) {
    savePeerToNvs();
  }
}

const char* LinkPhyEspNow::peerText() const {
  if (!mPeerKnown) return "(none)";
  return macText(mPeer);
}

void LinkPhyEspNow::loadPeerFromNvs() {
  Preferences prefs;
  if (!prefs.begin(kEspNowNvsNamespace, /*readOnly=*/true)) return;
  uint8_t tmp[kEspNowMacLen] = {0};
  const size_t n = prefs.getBytes(kEspNowNvsPeerKey, tmp, sizeof(tmp));
  prefs.end();
  if (n != kEspNowMacLen) return;        // 没有/长度不对 ⇒ 退化成广播，**不是错误**
  bool allZero = true;
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) {
    if (tmp[i] != 0u) { allZero = false; break; }
  }
  if (allZero) return;
  for (uint8_t i = 0; i < kEspNowMacLen; ++i) mPeer[i] = tmp[i];
  mPeerKnown = true;
  ++mNvsLoads;
  addPeerUnicast();                      // 下一次 pumpTx 就单播
}

void LinkPhyEspNow::savePeerToNvs() {
  if (mNvsSaves >= kMaxNvsSaves) return;   // 上界（见 kMaxNvsSaves 的说明）
  Preferences prefs;
  if (!prefs.begin(kEspNowNvsNamespace, /*readOnly=*/false)) return;
  const size_t n = prefs.putBytes(kEspNowNvsPeerKey, mPeer, kEspNowMacLen);
  prefs.end();
  if (n == kEspNowMacLen) ++mNvsSaves;
}

// ============================================================
// dashlink::LinkPhy
// ============================================================

int LinkPhyEspNow::available() {
  if (!mOnline) return 0;
  return (int)((mRxHead + kRxRingBytes - mRxTail) % kRxRingBytes);
}

int LinkPhyEspNow::read() {
  if (!mOnline) return -1;
  if (mRxTail == mRxHead) return -1;         // 现在没有（非阻塞契约：不是错误）
  const uint8_t b = mRxBuf[mRxTail];
  mRxTail = (uint16_t)((mRxTail + 1u) % kRxRingBytes);
  return (int)b;
}

int LinkPhyEspNow::availableForWrite() {
  if (!mOnline) return 0;
  // 在途到顶 ⇒ 报 0（上层一个字节都不写）。这是**主动**背压：
  // 详见 .h 文件头第一条里那段"唯一一处诚实的保留"。
  if (mPending >= kMaxPending) return 0;
  const int ringRoom = (int)(kTxRingBytes - mTxCount);
  return ringRoom > 0 ? ringRoom : 0;
}

size_t LinkPhyEspNow::write(const uint8_t* data, size_t n) {
  if (!mOnline || data == nullptr || n == 0u) return 0u;
  // ★ 这里**不碰射频**（§1.2 ①②）：只往环里拷。
  //   ★★ 整帧进出：一段比环的剩余还长的字节**一个都不收**（返回 0 并计数）——
  //     收一半会让上层以为"这帧发出去了"，而对端收到一个永远补不齐的半帧。
  //     （链路帧最大 71 B，环 256 B ⇒ 正常路径上永远不会走到这里。）
  if (n > (size_t)(kTxRingBytes - mTxCount)) {
    mTxOverflow += (uint32_t)n;
    ++mTxDropFrames;
    return 0u;
  }
  for (size_t i = 0; i < n; ++i) {
    mTxBuf[(mTxHead + mTxCount) % kTxRingBytes] = data[i];
    ++mTxCount;
  }
  return n;
}

uint16_t LinkPhyEspNow::pumpTx(uint32_t now_ms) {
  // ★ 学习日志的**唯一出口**：这里是主循环上下文（回调里一行日志都不打，
  //   见 .h 文件头 ②）。放在函数最前面 ⇒ 学到之后**下一圈**就能在串口上看到。
  flushPeerLog();

  // ★★ **PHY 自己的计数器**（2026-09-27 追加；起因是一手实测的两轮教训）
  //
  //   第 1 轮：上板时只看得见 `LinkTx` 的 `link: tx=`（那是"往 PHY 环里写了多少"），
  //           **看不出射频到底发没发** ⇒ 在板上猜了半小时 ⇒ 加了这一行。
  //   第 2 轮：加了之后**只打一次**（开机那一下）⇒ 实测"跑几秒后两个方向同时停住"时，
  //           仍然看不出"**发送是从哪一刻开始失败的**"（`tx_fail`/`last_err` 都冻在开机值）。
  //   ⇒ 现在改成**周期性**（同 `link:` 那行的节拍：2 s），并且"**第一次发送失败**"
  //     立刻单独打一行（见下面 `mSendFailLogged` 那一段）。
  //   ★ 只在"这一窗口有动静"时打（有在途包、收过包、或出过错）—— 空闲不刷屏。
  if (mOnline && (uint32_t)(now_ms - mPhyLogAtMs) >= kPhyLogPeriodMs &&
      (mPending > 0u || mRxFrames > 0u || mTxSendFail > 0u || mTxFrames > 0u || mTxCount > 0u)) {
    mPhyLogAtMs = now_ms;
    const bool ever_rx = (mLastRxMs != 0u);
    const uint32_t gap_rx = (ever_rx && now_ms >= mLastRxMs) ? (uint32_t)(now_ms - mLastRxMs) : 0u;
    dash_logf("espnow: tx_frames=%lu tx_bytes=%lu tx_fail=%lu last_err=%d done=%lu done_fail=%lu "
              "pending=%u txring=%u | rx_frames=%lu rx_bytes=%lu rx_foreign=%lu overflow=%lu "
              "gap_rx=%lums\n",
              (unsigned long)mTxFrames, (unsigned long)mTxTotal, (unsigned long)mTxSendFail,
              (int)mLastSendErr, (unsigned long)mTxDone, (unsigned long)mTxStatusFail,
              (unsigned)mPending, (unsigned)mTxCount, (unsigned long)mRxFrames,
              (unsigned long)mRxTotal, (unsigned long)mRxForeign, (unsigned long)mRxOverflow,
              // ★ "射频静默多久了"：一条读数就把"本来就没包"与"射频停了"分开
              ever_rx ? (unsigned long)gap_rx : (unsigned long)0xFFFFFFFFu);
  }

  if (!mOnline || mTxCount == 0u) return 0u;
  if (mPending >= kMaxPending) return 0u;    // 在途满：这一圈不交（下一圈再来）

  // ★★ 交给谁（2026-09-27 改过一次，理由是一手实测）：
  //   学到对端 ⇒ **单播**给它的 MAC；还没学到 ⇒ **显式发广播地址**
  //   （`kEspNowBroadcast` 那 6 个 0xFF）。
  //   ★ 为什么**不再用 `nullptr`**：`esp_now_send(NULL, …)` 的口径是"发给 peer 表里
  //     **所有** peer"，那是 IDF 文档里一句话的事 —— 而"广播 peer 到底在不在表里"
  //     没法从 `addPeerBroadcast()` 的返回值看出来（重复添加本来就会返回
  //     `ESP_ERR_ESPNOW_EXIST`，我们把它当成功了）。上板实测时**没有任何一帧出去**，
  //     而"显式地址"这条路径的判据是硬的：地址不在表里 ⇒ 立马返回
  //     `ESP_ERR_ESPNOW_NOT_FOUND`（那个错误码会进 `mLastSendErr` 并打出来）。
  //   ⇒ 显式广播的另一个好处：**失败可见**（`nullptr` 那条路失败时是静默的）。
  const uint8_t* dst = mPeerKnown ? mPeer : kEspNowBroadcast;

  uint16_t sent_bytes = 0;
  uint8_t packets = 0;
  // ★★ 一个 ESP-NOW 包 = **恰好一帧**（2026-09-27 上板实测后改；这是丢包率的真因）
  //
  //   老写法按"环里连续有多少字节"发（`chunk = min(mTxCount, 到环尾)`）⇒ 只要环里
  //   攒了 2~3 帧，**一个包就带上 2~3 帧**。发出去本身没问题（`tx_fail=0`、`done`
  //   跟得上），但接收回调的 `looksLikeV1Frame()` 用的是"一个包 = 一帧"的判据
  //   （`len == 7 + LEN` 是**等号**）⇒ 凡是打包了多帧的包，长度对不上，**整包被
  //   当成杂散包丢掉**（记 `rxForeign`）。
  //
  //   实测证据（两块板分开放、ch6、主板敲一次 `w`）：
  //     · 主板 `tx_frames` 41→8403、`tx_fail=0`、`done` 涨到 8289 ⇒ 射频**确实发出去了**；
  //     · 从板 PHY `rx_frames` 199→6068、`gap_rx=0..1ms` ⇒ 空中**也收到了**；
  //     · 可 `rx_foreign` 47→2134，测量只认了 609/1995 帧 ⇒ 丢在"判成外来包"这一步；
  //     · 且 `p50 = 20ms`（发送周期 10ms 的整数倍）⇒ 成批发的时候**整批**丢掉。
  //   ★ 这一条也解释了为什么"两块板分开摆"反而更差（36.75% → 69.47%）：成批的
  //     程度取决于主循环节奏与环里攒了多少，跟距离无关。
  //
  //   ⇒ 修法：**从环头解出下一帧的真实长度**（v1 帧自定长：7 + LEN，见 §2），只发
  //     这一帧；整帧还没齐就等下一圈。跨环接缝的帧先拷进栈上小缓冲再发 ——
  //     `esp_now_send()` 本来就会拷贝，多这一次拷贝是免费的，换来的是"**包 = 帧**"
  //     这条不变量在收发两侧都成立。
  uint8_t pkt[kFrameBytesMax];
  while (packets < kPumpPackets && mTxCount > 0u && mPending < kMaxPending) {
    if (mTxCount < (uint16_t)kOverhead) break;      // 连帧头都没齐：等下一圈
    uint8_t hdr[kOverhead];
    for (uint16_t i = 0; i < (uint16_t)kOverhead; ++i) {
      hdr[i] = mTxBuf[(mTxHead + i) % kTxRingBytes];
    }
    const uint8_t plen = hdr[kOffLen];
    if (hdr[kOffSync] != kSync || !lenInRange(plen)) {
      // 环里应当永远是整帧进出（`LinkTx::pump()` 按帧写、这里按帧取）⇒ 帧头必须自洽。
      // 不自洽说明上游写坏了：**丢一个字节重同步**，绝不把垃圾当帧发出去。
      ++mTxDropFrames;
      mTxOverflow += 1u;
      mTxHead = (uint16_t)((mTxHead + 1u) % kTxRingBytes);
      --mTxCount;
      continue;
    }
    const uint16_t flen = (uint16_t)(kOverhead + plen);
    if (mTxCount < flen) break;                     // 整帧还没齐：等下一圈
    for (uint16_t i = 0; i < flen; ++i) pkt[i] = mTxBuf[(mTxHead + i) % kTxRingBytes];

    // ★ esp_now_send 会把这一段**拷进驱动自己的待发队列**（IDF 的语义：
    //   "the data will be copied"）⇒ 我们下一圈就能复用环里的这些字节。
    const esp_err_t e = esp_now_send(dst, pkt, (size_t)flen);
    mLastSendErr = (int8_t)e;                  // ★ 最近一次的真实返回值（日志里打）
    if (e != ESP_OK) {
      // ★★ **第一次**失败立刻单独打一行（2026-09-27 上板实测之后追加）：
      //   实测那两个方向的症状是"开机 5~8 秒后同时停住，而 `tx=`（LinkTx 的账）
      //   还在涨" —— 那正是"环被排空、但包没真出去"的形态。
      //   可当时的计数器**只在开机打一次** ⇒ 看不到"失败是从第几毫秒开始的"。
      //   ⇒ 这一行就是那个答案：`at=` 是第一次失败的**时刻**，后面带当时的
      //     err 码 + 在途包数 + PHY 环里还剩多少字节（这三个数一起才说明白
      //     "是驱动队列满、还是 peer 没找到、还是环被灌住了"）。
      //   ★ 只打一次（`mSendFailLogged`）：失败可能每圈都发生，刷屏会把日志环打爆。
      if (!mSendFailLogged) {
        mSendFailLogged = true;
        mFirstSendFailAtMs = now_ms;
        mFirstSendFailErr = (int8_t)e;
        dash_logf("espnow: FIRST send fail err=%d pending=%u txcount=%u at=%lums (peer=%s)\n",
                  (int)e, (unsigned)mPending, (unsigned)mTxCount, (unsigned long)now_ms,
                  mPeerKnown ? macText(mPeer) : "broadcast");
      }
      // ★★ 这里**不重试、不等待**：整帧丢掉并计数，下一圈再说。
      //   为什么不能"重试到成功"：主循环每多花一毫秒，VAN 边沿采集就多一分
      //   丢边沿的风险（§1.2 引的那次实测教训）。丢帧比拖住主循环轻。
      ++mTxSendFail;
      mTxOverflow += flen;      // 字节账仍要对得上（丢掉的字节也算"没送出去的"）
      mTxHead = (uint16_t)((mTxHead + flen) % kTxRingBytes);
      mTxCount = (uint16_t)(mTxCount - flen);
      break;
    }
    mTxHead = (uint16_t)((mTxHead + flen) % kTxRingBytes);
    mTxCount = (uint16_t)(mTxCount - flen);
    mTxTotal += flen;
    sent_bytes = (uint16_t)(sent_bytes + flen);
    ++mTxFrames;
    ++packets;
    ++mPending;
  }
  return sent_bytes;
}

}  // namespace dashlink

#endif  // LINK_PHY_ESP_NOW

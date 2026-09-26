#pragma once

// ============================================================
// 双板链路协议 v1 —— **ESP-NOW 的 LinkPhy**（无线那一档）
//
// 契约：`ARCHITECTURE.md`「## 双板链路协议 v1 范围」§1.2（发送侧三条硬约束）。
// 本文件是 `lib/link/link_phy.h` 的**第三个实现**（前两个：真 UART 的
// `link_phy_uart.h`、空壳 `link_phy_null.h`）—— 接口逐字同形，**协议/消息表/
// CRC/既有用例一个字都没动**：换的只是"字节怎么过到对面去"。
//
// ------------------------------------------------------------------
// 一、为什么会有这一档（现场的一句话）
// ------------------------------------------------------------------
// 有线（UART0 的 43/44）在台面上卡住了：主板在发（第 9 脚 3.11↔2.95 V 跳动）、
// 线交叉且导通、信号到得了副板排针，但副板 `link rx bytes=0`。主假设是
// 「**插着 UART Type-C 的板，4Pin UART 被那颗 `FSUSB42UMX` 从 ESP32 上摘走**」
// （微雪 wiki 原话 + 车主台面实测，见 ARCHITECTURE §8.2 / docs/LINK-TWO-BOARD.md）。
// ⇒ 车主拍板：**再加一条无线链路**。ESP-NOW 跑在 WiFi 那半边射频上
//   （S3 **没有经典蓝牙**，只有 BLE 与 WiFi）。
//
// ------------------------------------------------------------------
// 二、★★ 三条纪律（与 UART 那一档逐字同一条；违反了不会有编译期信号）
// ------------------------------------------------------------------
//   ① **全部非阻塞**：`available()/read()/write()` 一个都不等。
//      · `write()` 只把字节**拷进自有环**，一个字节都不碰射频；
//      · 真的交给射频只有 `pumpTx()`（**只在主循环里调**），而它调的是
//        `esp_now_send()` —— 那个函数只把包拷进驱动待发队列就返回
//        （**不阻塞、不等回调**）；
//      · `availableForWrite()` = `min(环剩余, 在途余量)` ⇒ "返回 N 就写得进 N"。
//        ★ 唯一一处诚实的保留（写在最显眼处）：`esp_now_send()` 仍可能返回
//          `ESP_ERR_ESPNOW_NO_MEM`（驱动队列满）。那时这一帧**整帧丢掉并计数**
//          （`txSendFail`），绝不自旋等待 —— 见 `link_phy_espnow.cpp` 里那段。
//   ② **只有主循环写**：`esp_now_send()` 只在 `pumpTx()` 里调，
//      而 `pumpTx()` 只在主循环里调（§1.2 ①）。
//      ★ 接收回调（WiFi 任务上下文）**只做两件事**：把字节拷进入环、计数。
//        它**不格式化、不打日志、不发任何东西、不碰 LinkRx** —— 今天刚因为
//        "日志同步写"在别的路径上栽过（见 `link_phy_uart.h` 文件头那条实测）。
//   ③ **单次调用有上界**：一次 `pumpTx()` 最多交
//      `kEspNowPumpPackets` 个包；一次 `available()`/`read()` 只碰内存。
//      与 UART 那一档"一圈最多写多少"同一条口径（§1.3）。
//
// ------------------------------------------------------------------
// 三、★★ 寻址：**不用人去抄 MAC**（自配置）
// ------------------------------------------------------------------
//   ① 开机**不知道**对端地址 ⇒ 每一个包先发**广播**（`FF:FF:FF:FF:FF:FF`）；
//   ② 收到对端**任何一帧** ⇒ 从 `esp_now_recv_info_t::src_addr` 学到它的 MAC
//      （这就是"广播 HELLO → 对端回 HELLO"那一来一回的副产品：`HELLO` 本来就
//      §3 规定**双向**、上电 1 次 + 每 5 s 重发直到收到对端 HELLO）；
//   ③ 学到之后**立刻改单播**（更省射频、也不打扰同信道上的别人）；
//   ④ 学到的 6 个字节**存 NVS**（命名空间 `dash`、键 `enpeer`）⇒ 下次开机
//      直接单播，不必再等一轮广播；读不出来就当没有（退化成广播，**不是错误**）。
//   ⑤ 日志：`espnow: peer=xx:xx:xx:xx:xx:xx learned`（ASCII，人一眼能抄下来）。
//
//   信道：`kEspNowChannel`（默认 **6**，`link_phy_espnow_cfg.h` 是唯一出处）。
//   ★ **两端必须一致**：ESP-NOW 不跨信道，配错了的症状是"两端都打 rx=0"，
//     所以开通那一行日志**一定**把 `ch=` 打出来。
//
// ------------------------------------------------------------------
// 四、与 UART 那一档的差异（都要知道，不然会误判）
// ------------------------------------------------------------------
//   · `txPin()/rxPin()` 返回 **-1**（无线没有引脚）。调用方打印之前要先看
//     `port()` 是不是 `kNoPort`（= -1）—— `main.cpp` 那一行开机日志就是这么分的。
//   · `port()` 返回 `kNoPort`；`loopback()` 恒 false（无线没有回环这一说）。
//   · `baud()` 返回 `kEspNowAirBaud`（**约**数，只用于日志；见 .cpp 的说明）。
//   · 这一档**只在固件构建里编**（`LINK_PHY_ESP_NOW=1`）：宿主机上既没有
//     `esp_now_*` 也没有 `WiFi`。纯逻辑与常量在
//     `link_phy_espnow_cfg.h`（宿主机也编，用例钉它）。
// ============================================================

#include "link_phy_espnow_cfg.h"

#if LINK_PHY_ESP_NOW

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// ★★ 为什么这个头要 include `esp_now.h`（它按说只该出现在 .cpp 里）：
//   ESP-NOW 的两条回调**只能用全局函数指针注册**（`esp_now_register_recv_cb` /
//   `_send_cb`），所以"回调要从这里转发给唯一实例"这件事必须在本类里声明 ——
//   而那两个声明要用 IDF 的 typedef（`esp_now_send_cb_t` / `esp_now_recv_cb_t`）。
//   ⇒ 本文件**只在 `LINK_PHY_ESP_NOW=1` 的固件构建里被编到**（见上面的 `#if`），
//     而那种构建本来就已经把整个 WiFi 栈拉进来了（Flash 的代价如实记在 ACCEPTANCE.md），
//     所以这一步不会额外引入任何东西。
#include <esp_now.h>

#include "link_phy.h"

// ============================================================
// ★★ 编译期闸门：**无线链路的构建里，日志不许写 UART0**
//
// 为什么（与 link_phy_uart.cpp 里那道 `#error` 是**同一条判断**，只是主体换了）：
//   这份固件要么把 UART0 当数据面（UART 那一档），要么**根本不用它**。
//   只要这份固件里有**任何**一条链路 PHY，`van_replay_poll()` 与 `Serial0` 那一路
//   就应该按"UART0 不是我们的日志口"来配 —— 而现在那条判断的运行时落点是
//   `DASH_LOG_UART0`（`lib/dashcore/dash_log.h` 是唯一出处）。
//   ⇒ 谁要是在 ESP-NOW 的 env 里显式 `-DDASH_LOG_UART0=1`（把 UART0 又当日志口），
//     就在**编译期**拦住：那多半是"从 UART 档复制 env 时漏删了一行"，
//     而不是有意为之；真要有意，那也得先把"UART0 归谁"这件事想清楚。
//   ★ 用途不只是防呆：下一单上板时，**新 env 的日志口径**是"应该与
//     `esp32s3-rgb-master/slave` 逐字相同（都只走原生 USB-CDC）"——
//     这道闸门就是那句话的可执行形式。
// ============================================================
#if defined(DASH_LOG_UART0) && (DASH_LOG_UART0 != 0)
#error "link_phy_espnow: 这份固件有无线链路 PHY，却又把日志放回了 UART0(DASH_LOG_UART0!=0)。两板架构里 UART0(43/44)要么归链路、要么什么都不占；请去掉那个 -DDASH_LOG_UART0（默认就是 0 = 日志只走原生 USB-CDC）。"
#endif

namespace dashlink {

class LinkPhyEspNow : public LinkPhy {
 public:
  // ---- 与 LinkPhyUart 同形的访问器 ----
  // ★ 无线没有引脚：这两个返回 **-1**（"没有"），调用方按 `port() == kNoPort` 分。
  static const int8_t kNoPort = -1;
  static const int8_t kNoPin  = -1;
  // 空气速率（**约**数，只用于日志）：ESP-NOW 在 2.4 GHz 上按 1 Mbps 那一档
  // 的兼容速率发（见 .cpp 里那段说明）。**它不是**链路速率的上限依据 ——
  // 真正要看的数是 `meas rx:` 那几行（丢包/间隔/抖动）。
  static const uint32_t kEspNowAirBaud = 1000000u;

  // ---- 缓冲尺寸（与 UART 那一档同一口径：环只是"写永不阻塞"的手段） ----
  static const uint16_t kTxRingBytes = kEspNowTxRing;
  static const uint16_t kRxRingBytes = kEspNowRxRing;
  // 在途上限（见 cfg 头文件）与"一次 pumpTx 最多几个包"
  static const uint8_t kMaxPending  = kEspNowMaxPending;
  static const uint8_t kPumpPackets = kEspNowPumpPackets;
  static const uint8_t kChannel     = kEspNowChannel;

  LinkPhyEspNow() {}

  // 初始化。`loopback` 参数**为与 UART 那一档同形而保留**：无线没有回环，
  // 传 true 也不改变行为（会打一行日志说明，免得有人以为它生效了）。
  // ★ 只在 `setup()`/主循环里调（会起射频与任务），绝不在 ISR 上下文里调。
  void begin(bool loopback = false);

  // 真正的"交给射频"：**只在主循环里调**（§1.2 ①）。
  // 返回本次交给驱动的**整帧字节数**（0 = 环空 / 没到点 / 在途满）。
  // 单次调用最多交 `kPumpPackets` 个包，不忙等、不 delay。
  // ★★ `now_ms` 是**新增的参数**（2026-09-27 上板实测之后加的，默认值 0 = 不报时刻）：
  //   两块板上实测到"开机 5~8 秒后两个方向同时停住，而 `tx=` 还在涨" ⇒
  //   必须能回答"**第一次发送失败发生在第几毫秒**"。调用方把主循环的 `millis()`
  //   传进来即可（`main.cpp` 两处调用点都拿得到 `now`）。
  //   ★ 与 `LinkPhy` 接口的其余部分无关：`pumpTx()` 本来就不在基类接口上
  //     （它是"形状补齐"那一组，见 link_phy_null.h 的说明），所以加一个有默认值的
  //     参数对既有调用方**零影响**。
  uint16_t pumpTx(uint32_t now_ms = 0u);

  // ---- dashlink::LinkPhy ----
  int    available() override;
  int    read() override;
  int    availableForWrite() override;
  size_t write(const uint8_t* data, size_t n) override;
  bool   online() const override { return mOnline; }

  // ---- 与 LinkPhyUart 同形的形状补齐（见 link_phy_null.h 那段"为什么还要有"） ----
  int8_t   port() const { return kNoPort; }
  int8_t   txPin() const { return kNoPin; }
  int8_t   rxPin() const { return kNoPin; }
  bool     loopback() const { return false; }
  bool     started() const { return mOnline; }
  uint32_t baud() const { return kEspNowAirBaud; }
  // 给开机日志用的一句话（ASCII）：没有引脚时"TX=GPIO-1"那种输出没法读。
  const char* phyName() const { return "ESP-NOW"; }
  // 信道（= kChannel，开机那一行打它）：**两端必须一致**，配错就"两端都打 rx=0"。
  uint8_t channel() const { return kChannel; }
  // ★ 学习到的对端 MAC 的**可读形式**（`aa:bb:cc:dd:ee:ff`）——返回值指向内部
  //   静态缓冲，只在"刚学到 / 要打日志"那一刻有效（别存指针）。
  const char* peerText() const;

  // ---- 自检 / 用例判据（与 UART 那一档的名字对齐） ----
  uint32_t rxTotal() const { return mRxTotal; }        // 从射频收进环的总字节数
  uint32_t rxOverflow() const { return mRxOverflow; }  // 入环时没地方放而丢掉的字节
  uint32_t rxFrames() const { return mRxFrames; }      // 收下的**包**数
  uint32_t rxForeign() const { return mRxForeign; }    // 不是发给我们的包（单播指错/别人的网）
  uint32_t txTotal() const { return mTxTotal; }        // 真正交给驱动的总字节数
  uint32_t txFrames() const { return mTxFrames; }      // 真正交给驱动的**包**数
  uint32_t txOverflow() const { return mTxOverflow; }  // write() 时环满而丢掉的字节
  uint32_t txDroppedFrames() const { return mTxDropFrames; }  // 环放不下整帧而丢的帧数
  uint32_t txSendFail() const { return mTxSendFail; }  // esp_now_send 报错（含 NO_MEM）
  uint32_t txDone() const { return mTxDone; }          // 发送完成回调里成功的包数
  uint32_t txStatusFail() const { return mTxStatusFail; }  // 发送完成回调报失败的包数
  uint32_t peerLearnCount() const { return mPeerLearned; } // 学到/更新对端 MAC 的次数
  uint32_t nvsLoads() const { return mNvsLoads; }      // 开机从 NVS 读到 peer 的次数
  uint32_t nvsSaves() const { return mNvsSaves; }      // 学到后写 NVS 的次数
  uint8_t  pending() const { return mPending; }        // 在途包数（回调还没回来）
  uint16_t txPending() const { return mTxCount; }      // 还在环里等 pumpTx 的字节数
  bool     sentTaskMode() const { return mSentIsTask; }  // 发送完成回调是"任务上下文"吗
  // ★★ **最近一次 `esp_now_send()` 的真实返回值**（2026-09-27 追加）。
  //   为什么必须有它：上板实测时只看得见 `LinkTx` 的 `link: tx=`（"往 PHY 环里写了多少"），
  //   **看不出射频到底发没发**。`tx_fail` 那个计数只说"失败过几次"，说不清**为什么**
  //   （`ESP_ERR_ESPNOW_NOT_FOUND` = peer 不在表里 / `_NO_MEM` = 驱动队列满 /
  //    `_IF` = 接口不匹配 / `_CHAN` = 信道不匹配）—— 而这几个原因的修法完全不同。
  //   ⇒ 存下这个值（主循环日志里打出来），本地排查一眼就能对号。
  int8_t   lastSendErr() const { return mLastSendErr; }

  // ---- 给"测速/测丢包"用（见 link_meas.h）----
  // ★ 收端解析测量帧时要用到包边界：本 PHY 的**入环是整包进出**的
  //   （`kRxRingBytes` 里一个包一段），所以 `read()` 逐字节给出去之后，
  //   调用方只要按帧长解就行 —— 与 UART 那一档完全同形（同一条 LinkRx 路径）。
  //   "包"这个概念只在两个计数上露头（rxFrames / txFrames），不改变字节流语义。
  //
  // ★★ 测量帧的"**到达钩子**"（2026-09-27 深夜，本单第二步）：
  //   注册之后，接收回调会在**收到包的那一刻**（WiFi 任务上下文）把测量信封交给它，
  //   由它打时间戳。为什么必须在这一刻打：若在主循环里打（收帧闸门那条路），
  //   量到的"到达间隔"会混进**收端主循环被抢占**（实测 ~100ms），
  //   那就不是在量链路，而是在量我们自己的调度 —— 实测 `gap_max` 113ms / p99 42ms
  //   而同一轮的 `wire_gap_max` 只有 10ms（发端已经按节拍发了）。
  //   · 返回 true ⇒ 这一包**已被消费**，不再进 RX 环（主循环那一趟也省了）；
  //   · 回调里只做"形态判据 + 交给它"，**不格式化、不打日志**（同一贯纪律）；
  //   · 没注册（nullptr）时行为与以前**逐字节相同**。
  typedef bool (*MeasSinkFn)(const uint8_t* payload, uint8_t payload_len, uint32_t now_ms);
  static void setMeasSink(MeasSinkFn fn) { g_meas_sink = fn; }
  // 被到达钩子消费掉的**包数**（只增；用来证明测量帧没再走 RX 环）。
  uint32_t rxMeasConsumed() const { return mRxMeasConsumed; }

 private:
  // 发送完成回调（WiFi 任务上下文）—— 只做计数，绝不做别的事。
  // ★★ 这两个声明**写成完整签名**（而不是用 IDF 的 `esp_now_recv_cb_t` /
  //    `esp_now_send_cb_t` 那两个 typedef 来声明）。2026-09-27 实测教训：
  //    用 typedef 声明成 `static esp_now_recv_cb_t onRecvStatic;` 时，`.cpp` 里
  //    **按同样签名写定义**会被判成"两回事"（`error: no declaration matches`）——
  //    那是 C++ 里"函数指针 typedef 当声明符"的老坑，与 IDF 无关。
  //    ⇒ 头文件里写死签名（与当前 IDF 5.5 逐字一致），`.cpp` 里同样写死，
  //      两处一眼能对上；换 IDF 大版本时**只有这两处**要改，而且编译期会当场报出来。
  static void onSendDoneStatic(const esp_now_send_info_t* tx_info, esp_now_send_status_t status);
  static void onRecvStatic(const esp_now_recv_info_t* info, const uint8_t* data, int len);
  // ★ 测量帧的到达钩子（nullptr = 没注册 ⇒ 行为与以前逐字节相同）。见上面那一段说明。
  static MeasSinkFn g_meas_sink;

  void onSendDone(uint8_t status);
  void onRecv(const uint8_t* src, const uint8_t* data, int len);

  bool peerKnown() const { return mPeerKnown; }
  void setPeer(const uint8_t* mac);      // 记下（**不写 NVS** —— 见 flushPeerLog 的说明）
  void flushPeerLog();                   // 主循环里打"学到对端"那一行 + **这时才写 NVS**
  void loadPeerFromNvs();
  void savePeerToNvs();
  bool addPeerBroadcast();
  bool addPeerUnicast();

  bool mOnline      = false;
  // ★ **这两个刻意不标 volatile**（与 `mRxHead` 那两个不同，理由要说得清）：
  //   `mPeerKnown`/`mPeer` 虽然也是"回调写、主循环读"，但它们**只在函数调用之间传递**
  //   （`onRecv()` 写、`pumpTx()` 读），而两次读之间必然夹着 `esp_now_send()` 这个
  //   外部函数调用 ⇒ 编译器没有把它们提出去的余地，`volatile` 只会带来
  //   "`volatile uint8_t*` 不能转成 `const uint8_t*`"这一类纯语法摩擦（实测 6 处报错）。
  //   ⇒ 判据：**只有当这个量被内联进紧密循环里当判据时**才必须 volatile
  //     （`available()`/`read()` 里的 `mRxHead` 正是那种）。
  bool mPeerKnown = false;
  bool mSentIsTask  = false;
  // ★ "学到对端"那一行的**延迟打印**标记：学习发生在 WiFi 任务的接收回调里，
  //   而**回调里不许打日志**（同步写 USB-CDC 会拖住射频任务）⇒ 记下来，
  //   由下一次主循环的 `pumpTx()` 打出去。
  bool mPendingLogPeer = false;
  uint8_t mLogCount = 0;                 // 那一行最多打 2 次（学到 / 改口），别刷屏
  uint8_t mPeer[kEspNowMacLen] = {0};
  // ★ `mPending` 由**发送完成回调**（WiFi 任务）减、主循环在 `while` 的**判据位置**读
  //   （"在途到顶就不发"那条）⇒ 与 `mRxHead` 同一条理由，标 volatile。
  volatile uint8_t mPending  = 0;

  // 出方向（write → pumpTx → esp_now_send）
  uint8_t  mTxBuf[kTxRingBytes] = {0};
  uint16_t mTxHead  = 0;
  uint16_t mTxCount = 0;
  uint32_t mTxTotal = 0;
  uint32_t mTxFrames = 0;
  uint32_t mTxOverflow = 0;
  uint32_t mTxDropFrames = 0;
  uint32_t mTxSendFail = 0;

  // 入方向（接收回调 → read）
  // ★★ 哪几个是**跨上下文**的量，逐条说清（2026-09-27，车主复核时点出的一处自相矛盾）：
  //   · **`mRxHead`**：接收回调（**WiFi 任务**上下文）写它、主循环读它 ⇒
  //     `available()` / `read()` 就是**只靠这两个下标**判断"有没有数据"的
  //     ⇒ 必须 `volatile`。不加的后果是**合法优化**造成的静默故障：`read()` 被内联进
  //     `while` 之后，把 `mRxHead` 的读**提到循环外**在单线程内存模型下完全合法
  //     ⇒ 症状恰好是我们最想抓的那一种："`link rx bytes=0 frames=0`，而对面在发"。
  //   · **`mRxTail`**：只有主循环写它 ⇒ 严格说不需要，但**读 `mRxHead` 的同时也在读它**
  //     （两者一起构成"有没有数据"这条判据）⇒ 一起标上，免得下一个人以为"漏了一个"。
  //   · **`mRxBuf` 本体不标**：它是**载荷**，不是判据。判据（下标）先于数据可见这条
  //     顺序由两个 volatile 写之间的顺序保证；`volatile` 不是同步原语（单生产者 +
  //     单消费者 + 下标先行，这个模式够用；与 `link_meas.h` 的队列同一条口径）。
  //   · 下面那几个**计数**同样由回调写、主循环读 ⇒ `volatile`。
  //   · ★ 出方向那一组（`mTx*`）**不需要**：`write()`/`pumpTx()` 都只在主循环里跑，
  //     回调（`onSendDone`）只碰 `mPending`/`mTxDone`/`mTxStatusFail` ——
  //     后两个已在下面标了 volatile，`mPending` 见它自己那行的说明。
  uint8_t  mRxBuf[kRxRingBytes] = {0};
  volatile uint16_t mRxHead = 0;
  volatile uint16_t mRxTail = 0;
  volatile uint32_t mRxTotal = 0;
  volatile uint32_t mRxOverflow = 0;
  volatile uint32_t mRxFrames = 0;
  volatile uint32_t mRxForeign = 0;
  // ★ 到达钩子消费掉的包数（只有接收回调写，主循环只读 ⇒ volatile）
  volatile uint32_t mRxMeasConsumed = 0;
  volatile uint32_t mTxDone = 0;
  volatile uint32_t mTxStatusFail = 0;

  uint32_t mPeerLearned = 0;
  // ★★ "这个对端确实带来过**形态合法的 v1 帧**"的计数（2026-09-27 追加）。
  //   它是"NVS 什么时候才写"这条判据的锚点：**只有它 > 0 才写**（见 flushPeerLog 的说明）。
  volatile uint32_t mPeerFrames = 0;
  uint32_t mNvsLoads = 0;
  uint32_t mNvsSaves = 0;
  // ★ 最近一次 `esp_now_send()` 的返回值（主循环读、主循环写 ⇒ 不需要 volatile）
  int8_t   mLastSendErr = 0;
  // ---- 下而三个是"上板实测跑几秒就停"那一单补的可观测性（2026-09-27）----
  // ★ 计数器那一行从"开机一次"改成**周期性**（同 `link:` 那行的节拍，2 s）：
  //   否则"发送从哪里开始失败"根本看不见（实测就是这么卡住的）。
  uint32_t mPhyLogAtMs = 0;          // 上一次打那一行的时刻
  // ★ **第一次发送失败**要立刻单独打一行，并带上当时的时刻/在途/环内字节数。
  bool     mSendFailLogged = false;
  uint32_t mFirstSendFailAtMs = 0;
  int8_t   mFirstSendFailErr = 0;
  // ★ 最近一次**收到包**的时刻（0 = 还没收到过）⇒ 汇总行能报"射频静默多久了"
  volatile uint32_t mLastRxMs = 0;
};

}  // namespace dashlink

#endif  // LINK_PHY_ESP_NOW

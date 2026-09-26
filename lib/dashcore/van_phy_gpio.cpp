#include "van_phy_gpio.h"
#include "dash_log.h"   // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC(见文件头)

// 只有设备端编译(见头文件说明:本文件依赖 attachInterrupt / esp_timer_get_time)
#if defined(ARDUINO)

#include <Arduino.h>

#if !defined(VAN_RX_PIN)
// ★★ 2026-09-27 晚：默认值从 **16** 改成 **44**（`PINOUT.md` / `ARCHITECTURE.md` 同步改）。
//
//   为什么改（这是"接线定义要重新理一遍"那一条的落地）：
//     · 车主手上这块 **2.8C 非触控版**只把 12PIN 排针引出来，那 12 个脚是
//         `GP0 / GND / RXD / TXD / SDA / SCL / 3V3 / GND / D+ / D- / 5V / GND`
//       —— **16 根本不在里面**（它在板上是空的，但要焊飞线才够得着）。
//     · 排针上**能**当自由脚用的只有 `GP0`(strapping，不用) 与 `RXD`/`TXD`
//       （= **GPIO44 / GPIO43**，微雪 wiki 原话："can be used as a regular GPIO"）。
//       15/7 是板载 I2C（TCA9554/QMI8658/PCF85063），19/20 是原生 USB。
//     · 两颗之间选 **44**（排针的 `RXD`）：UART 的约定里它是**输入**那一侧 ——
//       万一将来有人在这块板上又打开了 UART0，44 只会被"读"（不顶牛），
//       而 43 会被 UART0 当**输出**驱动，与收发器的 `RO` 推挽输出**直接对顶**
//       （见下面那道编译期闸门）。
//   ★ 前提（现场必须满足，否则 43/44 与排针是**断开**的）：**UART Type-C 不插**。
//     板载 `FSUSB42UMX` 按"UART Type-C 插没插"选边：插了 ⇒ 43/44 归 CH343P、
//     排针那两针失效；没插 ⇒ 排针那两针通到芯片。我们的日志+供电走**原生 USB**
//     （GPIO19/20，`VID_303A`），所以正常情况下正好满足。
//   ★ 这条与"日志口"是同一个约束：ESP-NOW 那两份 env 里 UART0 本来就不打日志
//     （`DASH_LOG_UART0=0`，`lib/link/link_phy_espnow.h` 里还有一道 `#error` 守着），
//     现在再由下面那道闸门把"VAN 收在 43/44"与"链路占 43/44"这两种构建**编译期**分开。
#define VAN_RX_PIN 44
#endif

// ★★ 编译期闸门（2026-09-27）：**VAN 收在 43/44 与"链路占 UART0"不能共存**。
//   为什么必须有它：这两件事分别在两个 env 里都对，合起来就是**硬件对顶** ——
//   有线链路那一档（`LINK_PHY_UART=1`）把 43 当 UART0 的 TX **输出**、
//   44 当 RX；而 VAN 的收发器 `RO` 是**推挽输出**。谁把 VAN 也放在 43 上，
//   两个输出就同时驱同一根线（可能只是多耗电，也可能是其中一颗的驱动级发热），
//   而且**症状完全不指向接线**（表现成"链路偶尔丢帧"或"VAN 解不出帧"）。
//   本款两块 2.8C 跑的是**无线**那一档（`LINK_PHY_UART=0`）⇒ 正常情况下碰不到这条；
//   这条守的是"将来有人拿这份代码去编有线那一档"。
#if defined(LINK_PHY_UART) && (LINK_PHY_UART != 0) && \
    (VAN_RX_PIN == 43 || VAN_RX_PIN == 44)
#error "van_phy_gpio: VAN 接收脚(43/44)与有线链路(UART0)撞在同一根线上 —— 有线那一档的 43 是 UART0 的 TX 输出,而收发器的 RO 也是推挽输出,两个输出会对顶. 两条出路择一: (1) 这块板用无线那一档(-DLINK_PHY_UART=0,默认的两份 *-now env 就是); (2) 把 VAN 接收脚换到不冲突的脚(-DVAN_RX_PIN=<n>). 见 PINOUT.md 的 2.8C 接线一节."
#endif

// ★ 极性翻转开关(2026-09-22 新增,编译期)
//
// ★★ **先说结论(2026-09-22 实测):本项目的极性就是「反」的。
//    遇到 "edges 正常涨、frames=0",第一处置是「把 9004/9005 两根线对调」——
//    几秒钟的事,不要先去动固件、也不要先怀疑解码器。**
//    实测:对调前 edges 3478/s(与真总线基线吻合)但 frames=0;
//         对调后 11 秒 492 帧、fcs_ok==frames(CRC 全过)。
//
// 那这个宏留着干什么:**在"没把握是不是极性"时做对照确认** ——
//   有些场合不方便立刻动车上的线(比如线已包好、或在台架上用罐头信号),
//   这时用 -DVAN_RX_INVERT=1 烧一版就能判定,不用碰接线。
//   ★ 它是**诊断手段,不是修法**:确认极性之后,正式做法是对调线,然后回到
//     默认的 [env:esp32s3] —— 长期带着这个宏,下次换车/换线就埋了个坑。
//
// 为什么症状这么有迷惑性:VAN 是差分曼彻斯特,收发器 RO 给的是**单端**电平,
// "显性/隐性对应 0 还是 1"取决于 9004/9005 哪根进了 A、哪根进了 B。
// 反相后 SOF 图案 0000111101 永远匹配不上 ⇒ 一帧都起不来,
// 而**边沿计数完全不受影响** —— 所以"边沿数正常"完全不能证明极性对。
// (ACCEPTANCE.md:169 那句"它只证明线通、芯片活,不证明极性/速率/协议"就是这个意思。)
//
// 用法:编译时加 -DVAN_RX_INVERT=1(见 platformio.ini 的 [env:esp32s3-vaninv])。
#if !defined(VAN_RX_INVERT)
#define VAN_RX_INVERT 0
#endif

// 总线空闲多久算"这一帧结束"。
//
// ★ 70µs 是**量出来**的,不是拍的(2026-09-19;工具 tools/van-decode/gap_stats.py,
//   输入 drive5min.csv 300s / 1,119,625 条跳变 / 17106 帧)。门限被夹在两个
//   实测数之间,两头都是硬的:
//     · **下界**:帧内最长的一段"没有跳变"= **49.0µs**
//       (6 个 8.25µs 的槽,17105 帧里出现 1232 处)。门限比它小就会在帧中间
//       finish(),帧被截断 —— FCS 必不过,表现是"edges 在涨、frames 不涨"。
//     · **上界**:最短的帧间空闲 = **95.5µs**(12 槽)。门限比它大就会把背靠背
//       的两帧并进同一个解析器缓冲,丢一帧(旧值 300µs 正是如此:17105 个帧
//       边界里 **481 个 < 300µs**,占 2.81% —— 每处并帧丢一帧)。
//     · 实测直方图里 **7~11 槽(53.6~94.9µs)是空档**:帧内间隔全在它下面、
//       帧间空闲全在它上面。70µs 落在空档里,距两侧 +21.0µs / -25.5µs,
//       也正好是两侧的几何中点(√(49.0×95.5) = 68.4)。
//   ★ 为什么不是 60µs(虽然它也在空档里):60µs 只容得下 7.2 个槽的帧内间隔,
//     一个 8 槽的同电平段(66µs)就会被误判成帧尾;70µs 容得下 8.4 个槽,
//     留出的余量能兜住槽时间的实测漂移(8.1076~8.3212µs,见 fit_fcs.py)。
//   ★ 这个值**固定,不随时基缩放**:它只表示"总线静了多久算帧尾",与槽时间
//     无关。换了车/换了抓包要**重新量**这两个数,不是按比例缩放 kTsNs。
//   复现:python tools/van-decode/gap_stats.py <抓包.csv>
static const uint32_t kIdleCloseUs = 70;

// 一秒报一次"有没有边沿/丢没丢",实车调线时它是判断"接对没有"的第一手信息:
//   · edges 一直是 0            → RO 没信号:极性反了 / 没接 / 收发器没供电
//   · edges 在涨但 frames 不涨  → 收到的是噪声或有信号但解不出帧(波特率/极性)
//   · dropped 在涨              → 中断太密,主循环排空太慢(考虑改 RMT,见文末)
static const uint32_t kReportMs = 1000;

static VanPhyGpio* g_isr_owner = nullptr;   // 中断跳板需要它

static void IRAM_ATTR van_isr_thunk() {
  if (g_isr_owner) g_isr_owner->onIsrEdge();
}

void VanPhyGpio::onIsrEdge() {
  // ★ ISR 里只做这两件事:读电平、读时间戳(微秒),然后入队。
  //   · esp_timer_get_time() 是 ISR 安全的(直接读硬件计数器),比 micros() 稳;
  //   · 绝不在中断里调用解码器(几百微秒,会丢边沿 —— 见 van_edge_queue.h)。
  const uint32_t t = (uint32_t)esp_timer_get_time();
#if VAN_RX_INVERT
  // 极性翻转:见本文件开头 VAN_RX_INVERT 的说明
  q_.push(t, (uint8_t)(digitalRead(VAN_RX_PIN) ? 0 : 1));
#else
  q_.push(t, (uint8_t)(digitalRead(VAN_RX_PIN) ? 1 : 0));
#endif
}

void VanPhyGpio::begin() {
  wire_.begin();
  q_.reset();
  last_edge_us_ = (uint32_t)esp_timer_get_time();
  frames_emitted_ = 0;
  // ★ 上拉（2026-09-27 改）：原来是裸 `INPUT`。为什么改成 `INPUT_PULLUP` ——
  //   收发器**还没接**的时候（上车第一件事就是这种状态），悬空脚会被噪声拉出
  //   一串边沿，`van: edges=` 一直涨 ⇒ 看起来像"总线活着"，而其实一根线都没接。
  //   那是**最误导人**的一种读数（PINOUT.md B 节第 5 条那套诊断法——
  //   "静止 15 秒边沿不涨、摸一下才跳一大截"——**正是靠这个静止基线**）。
  //   上拉之后：没接 = 恒定高 = 0 边沿（干净基线）；接了 = 收发器的推挽输出
  //   直接驱动这根脚，45kΩ 的内部上拉可以忽略。
  //   ★ 只涉及"没接时读什么"，不动任何解码逻辑；本改动**没有**用真收发器验过
  //   （手上没有活总线），验过的是"编译通过 + 回放链不受影响"。
  pinMode(VAN_RX_PIN, INPUT_PULLUP);
  g_isr_owner = this;
  // CHANGE:上升+下降都要。VAN 是差分曼彻斯特(每个位都有跳变),
  // 只抓一个方向会丢一半槽。
  attachInterrupt(digitalPinToInterrupt(VAN_RX_PIN), van_isr_thunk, CHANGE);
  dash_logf("van phy: gpio 就绪 RX=GPIO%d(RO),空闲 %uus 关帧,极性%s\n",
                VAN_RX_PIN, (unsigned)kIdleCloseUs,
#if VAN_RX_INVERT
                "**已翻转**(VAN_RX_INVERT=1)");
#else
                "正常(VAN_RX_INVERT=0)");
#endif
}

void VanPhyGpio::tick(uint32_t now_ms) {
  // 1) 排空队列 → 解码器。一批一批地做,别把主循环卡在这里:
  //    一帧几百个边沿,而每个边沿的解码只有几十微秒,所以整帧也就几毫秒。
  //    budget = 一整圈:吃到这么多说明队列已经满了(dropped 在涨),
  //    这时剩下的边沿下一 tick 接着喂 —— 下面的关帧判据会等它。
  VanEdgeQueue::Edge e;
  uint16_t budget = VanEdgeQueue::kCapacity;
  while (budget-- && q_.pop(&e)) {
    last_edge_us_ = e.t_us;
    wire_.onEdge(e.t_us, e.level != 0);
  }

  // 2) 总线空闲 → 关帧。★ 判据在 van_edge_queue.h 的 vanIdleCloseReady():
  //    **队列非空就不关** —— 否则会把还压在队列里的同帧后续边沿截掉
  //    (表现是 edges 在涨、frames/fcs_ok 不涨)。
  //    用 micros() 而不是 tick 的 now_ms:边沿时间戳是微秒域,两者混用会差三个数量级。
  const uint32_t now_us = (uint32_t)esp_timer_get_time();
  if (vanIdleCloseReady(wire_.framePending(), q_.empty(), now_us, last_edge_us_,
                        kIdleCloseUs)) {
    if (wire_.finish()) ++frames_emitted_;   // finish() 内部会回调 sink 报包
  }

  // 3) 每秒一行诊断(只在有动静或丢包时打,免得刷屏)
  const VanPhyWire::Stats& st = wire_.stats();
  if ((uint32_t)(now_ms - last_report_ms_) >= kReportMs) {
    last_report_ms_ = now_ms;
    const uint32_t qd = q_.dropped();
    static uint32_t last_edges = 0, last_qd = 0;
    if (st.edges != last_edges || qd != last_qd) {
      dash_logf("van: edges=%u frames=%u fcs_ok=%u dropped=%u(队列%u) 待收=%u\n",
                    (unsigned)st.edges, (unsigned)st.frames, (unsigned)st.frames_fcs_ok,
                    (unsigned)st.frames_dropped, (unsigned)qd, (unsigned)wire_.pendingBytes());
      last_edges = st.edges;
      last_qd = qd;
    }
  }
}

// ------------------------------------------------------------
// 如果以后 dropped 一直涨(实车 + WiFi 同开时会遇到),正确的升级方向是
// **RMT 外设**:它能在硬件里记录每个边沿的时长,CPU 完全不参与,
// 再把时长序列换算成边沿喂给同一个 VanPhyWire —— 解码那一半一行都不用改。
// RMT RX 的坑:通道数有限(8 个)、需要 DMA 缓冲、换算要注意 RMT 的 tick 周期。
// 现在先不上:GPIO 中断这条路已经够把车速那一帧抓下来,先拿到数据再说。
// ------------------------------------------------------------

#endif  // ARDUINO

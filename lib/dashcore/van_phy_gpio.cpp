#include "van_phy_gpio.h"
#include "dash_log.h"   // 日志同时打到 USB-CDC 与 UART0(见文件头说明)

// 只有设备端编译(见头文件说明:本文件依赖 attachInterrupt / esp_timer_get_time)
#if defined(ARDUINO)

#include <Arduino.h>

#if !defined(VAN_RX_PIN)
#define VAN_RX_PIN 16        // PINOUT.md:RO → GPIO16(UART2 RX 那一路)
#endif

// 总线空闲多久算"这一帧结束"。
//
// 依据:van_wire 的 EOF 判据是"连续 8 个 recessive 槽" = 64µs(1 槽 = 8µs)。
// 所以只要空闲超过 64µs 就说明帧尾已经到了;这里取 300µs 留足余量 ——
// 判晚一点**不会丢数据**(字节已经在解析器缓冲里,finish() 只负责关帧),
// 判早一点才会把帧截断,所以宁可保守。
static const uint32_t kIdleCloseUs = 300;

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
  q_.push(t, (uint8_t)(digitalRead(VAN_RX_PIN) ? 1 : 0));
}

void VanPhyGpio::begin() {
  wire_.begin();
  q_.reset();
  last_edge_us_ = (uint32_t)esp_timer_get_time();
  frames_emitted_ = 0;
  pinMode(VAN_RX_PIN, INPUT);
  g_isr_owner = this;
  // CHANGE:上升+下降都要。VAN 是差分曼彻斯特(每个位都有跳变),
  // 只抓一个方向会丢一半槽。
  attachInterrupt(digitalPinToInterrupt(VAN_RX_PIN), van_isr_thunk, CHANGE);
  dash_logf("van phy: gpio 就绪 RX=GPIO%d(RO),空闲 %uus 关帧\n",
                VAN_RX_PIN, (unsigned)kIdleCloseUs);
}

void VanPhyGpio::tick(uint32_t now_ms) {
  // 1) 排空队列 → 解码器。一批一批地做,别把主循环卡在这里:
  //    一帧几百个边沿,而每个边沿的解码只有几十微秒,所以整帧也就几毫秒。
  VanEdgeQueue::Edge e;
  uint16_t budget = VanEdgeQueue::kCapacity;   // 一次 tick 最多处理一整圈
  while (budget-- && q_.pop(&e)) {
    last_edge_us_ = e.t_us;
    wire_.onEdge(e.t_us, e.level != 0);
  }

  // 2) 总线空闲 → 关帧。★ 用 micros() 而不是 tick 的 now_ms:
  //    边沿时间戳是微秒域,两者混用会差出三个数量级。
  const uint32_t now_us = (uint32_t)esp_timer_get_time();
  if (wire_.framePending() && (uint32_t)(now_us - last_edge_us_) > kIdleCloseUs) {
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

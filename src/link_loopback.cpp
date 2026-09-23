// ============================================================================
//  双板链路 v1 —— **单板回环验证固件**（`esp32s3-linkloop` env 专用）
// ============================================================================
//  目的：在**一块**板子上把 `lib/link` 的收/发整条链跑一遍，不需要第二块板、不需要
//  真实双板接线。接线只有一根短路线（见 docs/LINK-LOOPBACK.md）：
//
//      ★ 把 **GPIO17 与 GPIO18** 短接（一根杜邦线/一根回形针都行）
//      然后：python -m platformio run -e esp32s3-linkloop -t upload --upload-port COM4
//      看串口（115200）：应当看到 linkloop: 那一段汇总（发送数/收到数/CRC 错/丢帧），
//      最后一行是 **linkloop: PASS**。
//
//  ★ 为什么不用 43/44 回环（用户口径 + §8 L1 的结论）：
//    裸 S3 devkit 上 43/44 接着**板载 USB-串口桥**（CH340/CH343P），而 §8 L1 记录的
//    那颗 `FSUSB42UMX` 是"二选一"的模拟开关 —— 它**摘不掉**桥。短接 43/44 等于把桥的
//    推挽 TX 一起并进回路（对打），测出来的东西不可信。所以本固件走 **UART1 + 17/18**
//    （`link_phy_pins.h` 的默认回环脚），43/44 一根都不碰。
//
//  ★ 这个固件测什么、不测什么（写清楚，免得把结论读过头）：
//    测：  ① 非阻塞 UART PHY 真的能把帧发出去（TX 环 → pumpTx → UART FIFO）；
//          ② 真的能收回来（UART 的 RX 任务 → PHY 环 → LinkRx::poll 解帧）；
//          ③ CRC 覆盖/帧长/大端字节序在**真实 UART 的字节流**上站得住；
//          ④ 两个环的溢出计数（LinkTx 整帧丢、PHY 的 TX/RX 环满）都看得见。
//    不测：43/44 的电气（§8 L1 的 ⓐⓑⓒ）、跨板电平、双板的角色对账 —— 那些要两块板。
//
//  ★ 为什么整段在 `LINK_LOOPBACK_FIRMWARE` 里：它与 `src/main.cpp` 抢 `setup()`/`loop()`
//    （同一个 env 只能有一个），所以 platformio.ini 里用 `-DLINK_LOOPBACK_FIRMWARE=1`
//    把 main.cpp 那半边摘掉。这比 `build_src_filter = -<main.cpp>` 好在"少编一段
//    代码"而不是"少编一个文件"——IDE 里看得见，LDF 也不会因此少扫出依赖。
// ============================================================================

#if defined(LINK_LOOPBACK_FIRMWARE)

#include <Arduino.h>

#include "link_app.h"
#include "link_frame.h"
#include "link_msg.h"
#include "link_phy_pins.h"
#include "link_phy_uart.h"
#include "link_role.h"
#include "link_rx.h"
#include "link_time.h"
#include "link_tx.h"

namespace {

// ---- 期望（要填多少、等多久）----
// 每个类型先发 40 帧（一共 200 帧 ≈ 2.7 s 线时 @115200），发完再留 3 s 收尾。
// ★ 为什么不发更多：这个固件的目的是"能不能通"，不是压带宽；200 帧已经足够把
//   "首帧丢半截、后续全部对齐"这类重同步问题暴露出来（真丢字节的话 CRC 错会立刻涨）。
const uint16_t kFramesPerType = 40;
const uint32_t kDrainMs       = 3000;   // 发完之后再等这么久收尾
const uint32_t kReportMs      = 1000;   // 心跳一行（让人知道它还活着）

// ---- 统计 ----
// 为什么除了 LinkRx 的 stats() 还要自己记一套：LinkRx 的账是"解帧那一层"的
// （crc_err/bad_len/…），而 sent_* 是"我们真的往链路上塞了多少帧"的账。两者对不上
// 才是信息：比如 sent=40、got=39 ⇒ 中间掉了一帧（或环满丢了一帧）。
struct Counters {
  uint16_t sent[5] = {0, 0, 0, 0, 0};   // HELLO/TICK/DATA/STATUS/EVENT（见 typeIndex）
  uint16_t got[5] = {0, 0, 0, 0, 0};
  uint16_t tx_dropped = 0;              // LinkTx 整帧丢（环不够）
  uint16_t tx_overflow = 0;             // PHY 的 TX 环满（write() 少收的字节）
  uint16_t rx_overflow = 0;             // PHY 的 RX 环满（丢掉的字节）
  uint16_t payload_bad = 0;             // 解出来的字段与载荷字节对不上（字节序错）
};

uint8_t typeIndex(uint8_t type) {
  switch (type) {
    case (uint8_t)dashlink::MsgType::Hello:  return 0;
    case (uint8_t)dashlink::MsgType::Tick:   return 1;
    case (uint8_t)dashlink::MsgType::Data:   return 2;
    case (uint8_t)dashlink::MsgType::Status: return 3;
    case (uint8_t)dashlink::MsgType::Event:  return 4;
    default:                                 return 0xFF;
  }
}
const char* typeName(uint8_t idx) {
  switch (idx) {
    case 0: return "HELLO";
    case 1: return "TICK";
    case 2: return "DATA";
    case 3: return "STATUS";
    case 4: return "EVENT";
    default: return "?";
  }
}

dashlink::LinkPhyUart g_phy;
dashlink::LinkTx      g_tx;
dashlink::LinkRx      g_rx;
Counters              g_c;

// ---- 要发的那些帧（内容刻意"可自校验"）----
// ★ 载荷里填的是 (type, i) 派生的可预测字节。校验方式见 onFrame()：
//   收到之后**重新解析**，再把"解析出来的字段"与"载荷里的原始字节"对一遍 ——
//   字节序写反了、打包/解包不对称，都会在这一步露出来，而不是靠人眼看十六进制。
uint8_t seedByte(uint8_t type, uint16_t i, uint8_t k) {
  return (uint8_t)(type * 31u + i * 7u + k * 13u + 0x11u);
}

uint8_t payloadLen(uint8_t idx) {
  switch (idx) {
    case 0: return dashlink::kHelloLen;
    case 1: return dashlink::kTickLen;
    case 2: return dashlink::kDataLen;
    case 3: return dashlink::kStatusLen;
    default: return dashlink::kEventLen;
  }
}
uint8_t typeFor(uint8_t idx) {
  switch (idx) {
    case 0: return (uint8_t)dashlink::MsgType::Hello;
    case 1: return (uint8_t)dashlink::MsgType::Tick;
    case 2: return (uint8_t)dashlink::MsgType::Data;
    case 3: return (uint8_t)dashlink::MsgType::Status;
    default: return (uint8_t)dashlink::MsgType::Event;
  }
}

uint16_t enqueueOne(uint8_t idx, uint16_t i) {
  const uint8_t type = typeFor(idx);
  const uint8_t len = payloadLen(idx);
  uint8_t payload[dashlink::kLenMax];
  for (uint8_t k = 0; k < len; ++k) payload[k] = seedByte(type, i, k);
  if (!g_tx.enqueueFrame(type, payload, len, dashlink::kLocalRole)) {
    ++g_c.tx_dropped;
    return 0;
  }
  ++g_c.sent[idx];
  return (uint16_t)(dashlink::kOverhead + len);
}

// 收到一帧：先记类型，再把载荷与"应当是什么"对一遍。
void onFrame(const dashlink::Frame& f) {
  const uint8_t idx = typeIndex(f.type);
  if (idx == 0xFF) return;                       // 不该发生（帧层已挡掉未知 TYPE）
  ++g_c.got[idx];

  // 载荷自校验：拿"解析出来的字段"与"载荷里的原始字节"对一遍。
  // ★ 这是本固件最值钱的一条断言：大端写反、字节序不一致这类错**编译期完全看不出来**，
  //   只有在真实字节流上往返一次才会露出来。
  if (f.type == (uint8_t)dashlink::MsgType::Data) {
    dashlink::DataMsg m;
    if (!dashlink::unpackData(f.payload, f.len, &m)) { ++g_c.payload_bad; return; }
    const uint8_t hi = f.payload[0], lo = f.payload[1];
    if ((uint16_t)(((uint16_t)hi << 8) | lo) != m.rpm_raw) ++g_c.payload_bad;
    if (m.flags != f.payload[5]) ++g_c.payload_bad;
  } else if (f.type == (uint8_t)dashlink::MsgType::Tick) {
    dashlink::TickMsg m;
    if (!dashlink::unpackTick(f.payload, f.len, &m)) { ++g_c.payload_bad; return; }
    const uint32_t be = ((uint32_t)f.payload[0] << 24) | ((uint32_t)f.payload[1] << 16) |
                        ((uint32_t)f.payload[2] << 8) | (uint32_t)f.payload[3];
    if (be != m.tick_ms) ++g_c.payload_bad;
  } else if (f.type == (uint8_t)dashlink::MsgType::Status) {
    dashlink::StatusMsg m;
    if (!dashlink::unpackStatus(f.payload, f.len, &m)) { ++g_c.payload_bad; return; }
    const uint32_t be = ((uint32_t)f.payload[2] << 24) | ((uint32_t)f.payload[3] << 16) |
                        ((uint32_t)f.payload[4] << 8) | (uint32_t)f.payload[5];
    if (be != m.uptime_ms) ++g_c.payload_bad;
  } else if (f.type == (uint8_t)dashlink::MsgType::Event) {
    dashlink::EventMsg m;
    if (!dashlink::unpackEvent(f.payload, f.len, &m)) { ++g_c.payload_bad; return; }
    if (m.evt_id != f.payload[0] || m.face != f.payload[3]) ++g_c.payload_bad;
  }
}

enum class Phase : uint8_t { Send, Drain, Done };
Phase    g_phase = Phase::Send;
uint8_t  g_idx   = 0;                 // 当前在发第几类
uint16_t g_i     = 0;                 // 当前类型里第几帧
uint32_t g_next_send_ms = 0;
uint32_t g_phase_start_ms = 0;
uint32_t g_last_report_ms = 0;

void report(bool final_pass) {
  const uint16_t total_sent = (uint16_t)(g_c.sent[0] + g_c.sent[1] + g_c.sent[2] +
                                         g_c.sent[3] + g_c.sent[4]);
  const uint16_t total_got = (uint16_t)(g_c.got[0] + g_c.got[1] + g_c.got[2] +
                                        g_c.got[3] + g_c.got[4]);
  const dashlink::LinkRxStats& st = g_rx.stats();

  Serial.printf("linkloop: %s\n", final_pass ? "=== 汇总 ===" : "--- 心跳 ---");
  Serial.printf("linkloop: 发送 %u 帧 / 收到 %u 帧   (期望 %u)\n", (unsigned)total_sent,
                (unsigned)total_got, (unsigned)(kFramesPerType * 5u));
  for (uint8_t i = 0; i < 5; ++i) {
    Serial.printf("linkloop:   %-6s sent=%-3u got=%-3u\n", typeName(i),
                  (unsigned)g_c.sent[i], (unsigned)g_c.got[i]);
  }
  Serial.printf("linkloop: CRC 错 %lu / bad_len %lu / 未知类型 %lu / 重同步噪声 %lu 字节\n",
                (unsigned long)st.crc_err, (unsigned long)st.bad_len,
                (unsigned long)st.unknown_type, (unsigned long)st.noise_bytes);
  Serial.printf("linkloop: 丢帧: LinkTx 环满 %u / PHY-TX 环满 %u 字节 / PHY-RX 环满 %u 字节\n",
                (unsigned)g_c.tx_dropped, (unsigned)g_c.tx_overflow,
                (unsigned)g_c.rx_overflow);
  Serial.printf("linkloop: 载荷自校验失败 %u 处\n", (unsigned)g_c.payload_bad);
  Serial.printf("linkloop: PHY 统计 rxTotal=%lu txTotal=%lu\n",
                (unsigned long)g_phy.rxTotal(), (unsigned long)g_phy.txTotal());

  // ---- 判据（写死在这里，别只看"有输出就算过"）----
  const bool ok = (total_sent == kFramesPerType * 5u) && (total_got == total_sent) &&
                  (st.crc_err == 0u) && (st.bad_len == 0u) && (st.unknown_type == 0u) &&
                  (g_c.payload_bad == 0u) && (g_c.tx_dropped == 0u) &&
                  (g_c.tx_overflow == 0u) && (g_c.rx_overflow == 0u);
  if (final_pass) {
    Serial.printf("linkloop: %s\n", ok ? "PASS" : "FAIL");
    if (!ok) {
      Serial.printf("linkloop: 排查顺序 ① GPIO17/18 真的短接了吗(万用表通断) "
                    "② 串口 115200 8N1 ③ 有没有别的外设占着 17/18 "
                    "④ UART1 是否被 OBD 抢走(17/18 默认就是 OBD 那对脚!)\n");
    }
  }
}

}  // namespace

void setup() {
  // 这块固件**不调用 dash_log_begin()**：那是显示固件的双通道日志（见 dash_log.h），
  // 它会把 UART0 也开起来；回环只需要一条能看的输出，直接走 USB-CDC 的 Serial。
  Serial.begin(115200);
  delay(300);

  Serial.printf("\nlinkloop: 206 dash 双板链路 v1 —— 单板回环验证固件\n");
  Serial.printf("linkloop: 芯片=%s rev%d  编译期口径 LINK_ROLE=%d\n", ESP.getChipModel(),
                (int)ESP.getChipRevision(), (int)LINK_ROLE);
  Serial.printf("linkloop: ★ 请确认 **GPIO%d 与 GPIO%d 已短接**（就这一根线）\n",
                (int)dashlink::kLoopbackTxPinC, (int)dashlink::kLoopbackRxPinC);
  Serial.printf("linkloop: 回环走 UART%d；日志走 USB-CDC 的 Serial（不碰 43/44）\n",
                (int)dashlink::kLoopbackUartPort);

  g_rx.setLocalRole(dashlink::kLocalRole);
  g_phy.begin(true);   // true = 回环模式：UART1 + GPIO17/18（见 link_phy_uart.h）
  Serial.printf("linkloop: PHY 就绪 port=%d tx=GPIO%d rx=GPIO%d @%u 8N1\n",
                (int)g_phy.port(), (int)g_phy.txPin(), (int)g_phy.rxPin(),
                (unsigned)dashlink::kLinkBaud);

  g_next_send_ms = millis();
  g_phase_start_ms = millis();
  g_last_report_ms = millis();
}

void loop() {
  const uint32_t now = millis();

  switch (g_phase) {
    case Phase::Send: {
      // 发得不快：每 2 ms 塞一帧（链路 115200 ≈ 86.8 µs/字节，一帧 11~23 B ⇒
      // 0.95~2.0 ms）。这个节奏让 TX 环基本是空的 —— 本固件要测"通不通"，
      // 不是测"环满了会怎样"（那条由 native 用例 test_link_tx_* 覆盖）。
      if ((int32_t)(now - g_next_send_ms) >= 0) {
        g_next_send_ms = now + 2;
        enqueueOne(g_idx, g_i);
        if (++g_i >= kFramesPerType) {
          g_i = 0;
          if (++g_idx >= 5) {
            g_idx = 0;
            g_phase = Phase::Drain;
            g_phase_start_ms = now;
            Serial.printf("linkloop: 五类 × %u 帧已入队，开始收尾 %u ms\n",
                          (unsigned)kFramesPerType, (unsigned)kDrainMs);
          }
        }
      }
      break;
    }
    case Phase::Drain:
      if ((now - g_phase_start_ms) >= kDrainMs) g_phase = Phase::Done;
      break;
    case Phase::Done:
      report(true);
      // 停在这里，不再刷屏；复位/重新上电才会再跑一轮。
      break;
  }

  // ① 收（主循环里 poll，非阻塞）—— 一次最多 64 B，单次很短（§1.3）
  dashlink::Frame f;
  while (g_rx.poll(g_phy, &f)) onFrame(f);

  // ② 排水：`LinkTx` 的环 → PHY 的环 → UART 的 FIFO。两步都只走"能走的那些字节"。
  g_tx.pump(g_phy);
  g_phy.pumpTx();

  // ③ 心跳：每秒一行（也证明主循环没有被写串口拖住 —— §1.2 的原始教训）
  if (g_phase != Phase::Done && (now - g_last_report_ms) >= kReportMs) {
    g_last_report_ms = now;
    report(false);
  }

  // ④ PHY 的溢出计数（由 RX 任务 / write() 写，逐圈取一次快照）
  const uint32_t rx_ovf = g_phy.rxOverflow();
  const uint32_t tx_ovf = g_phy.txOverflow();
  g_c.rx_overflow = (uint16_t)(rx_ovf > 65535u ? 65535u : rx_ovf);
  g_c.tx_overflow = (uint16_t)(tx_ovf > 65535u ? 65535u : tx_ovf);
}

#endif  // LINK_LOOPBACK_FIRMWARE

#pragma once
// 设备日志：**默认同时**打到 USB-CDC（原生 USB 口）和 UART0（板载 CH340 那个 UART 口）；
// ★ 但**这份固件里有链路 PHY 时只打 USB-CDC** —— 见下面 `DASH_LOG_UART0`。
//
// 为什么两个口都要发 —— 2026-09-18 刷了二十多次板子才搞明白:
//   · 原生 USB 口走的是 USB-Serial-JTAG。esptool 的复位序列是
//     "DTR 拉低 IO0 → 脉冲 EN"(见 esptool/reset.py 的 USBJTAGSerialReset),
//     而 pyserial **打开串口时默认就把 DTR/RTS 拉高**(serialutil.py:
//     _rts_state = _dtr_state = True)→ 打开监视器这一下本身就等于
//     "复位进下载模式"。结果:程序根本没机会跑,串口一片空白,
//     看起来跟"固件有毛病"一模一样。
//   · 板载 CH340 那条路是**真的 UART0 + 真的 EN/IO0 复位线**:ROM、二级
//     bootloader、panic 的日志全在它上面,复位后**第一个字节**都抓得到。
//   所以两个都发:插哪个口都看得见。
//
// (经典 ESP32 那边没有 CDC_ON_BOOT,Serial 和 Serial0 是**同一个** UART0,
//  所以下面按 CDC_ON_BOOT 判断,避免同一行打两遍。)
//
// ============================================================
// ★★ `DASH_LOG_UART0` —— "UART0 那一路日志"的**唯一编译期开关**(2026-09-23 新增)
//
// 为什么必须有这个开关:上了两板架构之后 **UART0 的 43/44 就是板间链路**
// (契约 ARCHITECTURE.md §0「载体」),而 `dash_logf()` 一直是无条件 `Serial0.write()`
// ⇒ 日志文本会**混进链路数据流**(从板会拿它当 VAN 回放行去解,见 §2 的分流)。
// 这就是 §0 那条待办。现在它由这一个宏收口,**开关只有一处**:
//
//   · 默认值按**这份固件里有没有链路 PHY**判定(`LINK_PHY_UART` 是
//     `platformio.ini` 里链路固件才加的宏):
//       没有链路 PHY ⇒ `DASH_LOG_UART0 = 1` —— 老行为(两个口都发);
//       有链路 PHY   ⇒ `DASH_LOG_UART0 = 0` —— **UART0 一个字节都不写**,日志只走 USB-CDC。
//   · 于是:**VAN 采集那条路(不带链路 PHY)的日志行为一个字节都没变**,
//     而**显示/链路构建再也不用去 `platformio.ini` 里写那条"我承认日志占着 UART0"的
//     豁免宏** —— 它自己就满足了 `lib/link/link_phy_uart.cpp` 里那道编译期闸门。
//   · 想显式钉死(比如宿主机构建、或将来某个 env 要恢复双通道),在 env 里
//     `-DDASH_LOG_UART0=0/1` 即可 —— 宏里两条 `#ifndef` 保证了 env 优先。
//   · ★ **为什么默认走"安全那一侧"**:忘记定义这个宏的后果,两个方向不对称 ——
//     漏关 UART0 = 日志文本静默地混进链路(症状是"从板收到一堆解不开的字节");
//     而误关 UART0 = 少一个日志口(插原生 USB 照样看得见)。所以不确定时**关**。
//   · ★ 回环固件(`env:esp32s3-linkloop`)也定义了 `LINK_PHY_UART`(继承自
//     `[env:esp32s3]`),所以这里同样是 0 —— 而回环固件本来就不调用
//     `dash_log_begin()` / `dash_logf()`(它直接用 `Serial`,见 `src/link_loopback.cpp`)。
// ============================================================
// ★★★ 硬约定：**日志绝不许阻塞主循环**（2026-09-26 新增）
//
// 起因是一份**实测出来的根因**（`ARCHITECTURE.md` §7.5.6、`ACCEPTANCE.md` 同名那一节，
// 车主那句"长鸣一会儿，画面也卡住了"）：
//
//   `dash_logf()` 原来是**同步**的 —— 格式化完就当场 `Serial.write()`。
//   而 `Serial` 是 HWCDC(USB-Serial-JTAG),它的 `write()` 在**没人读**的时候
//   会一路撞驱动超时(`tx_timeout_ms = 100` × `max_consec_timeouts = 20`
//   ⇒ 单次调用最坏 ≈ 2 s,`cores/esp32/HWCDC.cpp`)。
//   实测:同一个镜像、同一次上电 —— 有人读时主循环 **3.3 万圈/秒**;
//   没人读的那 28 秒里**一共只跑了 2 圈**(20120 ms + 7689 ms)。
//   ★ 而车上的常态**就是"没人读"**(USB 主机永远不在)⇒ 装车即废。
//
// ⇒ 现在的形状(三条一起才成立,**少一条都不行**):
//
//   ① **`dash_logf()` 只格式化进环,一个字节都不写串口** —— 环满就**丢**并计数,
//      **绝不等待**(`dashlogring::Ring::write()` 是纯内存拷贝:没有循环等待、
//      没有 delay、没有串口 API)。
//   ② 排空由主循环**末尾**的 `dash_log_drain()` 做,而且**按预算**
//      (`kDrainBudgetBytes`)+ **写之前先问"能不能写"**(`availableForWrite()`):
//      报 0 就一个字节都不写、立刻返回 —— 于是"驱动超时"那条路**根本不会被走到**。
//   ③ 丢掉的字节**必须可观测**(`dash_log_stats()` → 摘要行里的 `log: drain=… drop=…`):
//     宁可丢日志,也不能堵主循环 —— 这是**有意的取舍**,别把它当 bug 修。
//
//   ★ 为什么是"整行丢"而不是"半行也塞":半行会把剩下的日志**结构打乱**
//     (一行被劈成两半,后面接的又是另一行的头),排障时最费时间的正是这个。
//     所以 `Ring::write()` 只在**整行放得下**时写入,放不下就整行丢 + 计数。
//   ★ 为什么日志可以丢:它是**诊断**通道,不是数据通道。数据(链路/快照/蜂鸣器)
//     一个字节都不走这里。诊断的价值在"有读数",不在"一条不漏"。
//   ★ 宿主机构建(native / pcpreview)仍然直接写 stdout —— 那里没有 USB-CDC、
//     也没有"没人读"这回事,保持原样(`#if defined(ARDUINO)`)。
// ============================================================

#include <stdint.h>
#include <stddef.h>

#ifndef DASHLOG_RING_BYTES
// 8 KB:够放下 ~80 行(每行 ~100 B)的突发;排空预算 512 B/圈 ⇒ "有人读"时
// 排空一整环不到 16 圈(实测一圈 ~30 µs,即 < 1 ms),而环本身只占 8 KB 内部 RAM。
#define DASHLOG_RING_BYTES 8192u
#endif

// 一行的最大长度(超了由 vsnprintf 截断,截断的那行照样进环)。
#ifndef DASHLOG_MAX_LINE
#define DASHLOG_MAX_LINE 320u
#endif

// 主循环**每圈**最多往串口倒多少字节(硬上限)。
// ★ 512 B 与链路收帧那条预算(`kLinkRxBytesPerLoop`)同一个量级、同一个理由:
//   "一圈的工作量必须有上界"。115200 线速下 512 B ≈ 44 ms,而"有人读"时
//   USB-CDC 是 MB/s 级 ⇒ 正常路径上永远用不到这个上限。
#ifndef DASHLOG_DRAIN_BUDGET
#define DASHLOG_DRAIN_BUDGET 512u
#endif

// ============================================================================
//  ① 环形缓冲 + ② 速率闸门(**纯内存、无 Arduino 依赖**,所以 native 用例直接钉着)
// ============================================================================
namespace dashlogring {

// 环形缓冲:字节环,**只在整段放得下时写入**,否则整段丢弃并计数。
// ★ 这里**没有任何等待/循环/重试** —— 这是"日志不许阻塞主循环"的第一道保证。
class Ring {
 public:
  void init(uint8_t* storage, size_t capacity) {
    m_buf = storage;
    m_cap = storage ? capacity : 0u;
    m_head = 0u;  // 读游标(最老的字节)
    m_len = 0u;   // 现有字节数
    resetStats();
  }

  void resetStats() {
    m_dropped_bytes = 0u;
    m_drop_count = 0u;
    m_written_bytes = 0u;
    m_drained_bytes = 0u;
    m_blocked_drains = 0u;
    m_hwm = 0u;
  }

  bool ready() const { return m_buf != nullptr && m_cap > 0u; }
  size_t capacity() const { return m_cap; }
  size_t size() const { return m_len; }
  size_t space() const { return m_cap - m_len; }

  // 写入:返回真的写进去的字节数(**0 = 整段被丢**)。
  // ★ 语义是"要么整段进去,要么一个字节都不进" —— 见文件头那条"整行丢"的理由。
  size_t write(const void* data, size_t n) {
    if (n == 0u) return 0u;
    if (!ready() || n > m_cap - m_len) {
      ++m_drop_count;
      m_dropped_bytes += n;
      return 0u;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const size_t tail = (m_head + m_len) % m_cap;
    const size_t first = (n < m_cap - tail) ? n : (m_cap - tail);
    for (size_t i = 0; i < first; ++i) m_buf[tail + i] = p[i];
    for (size_t i = first; i < n; ++i) m_buf[i - first] = p[i];
    m_len += n;
    m_written_bytes += n;
    if (m_len > m_hwm) m_hwm = m_len;
    return n;
  }

  // 排空:**最多** budget 字节,交给 sink。sink 返回 0 = "现在写不出去",
  // 立刻停手(剩下的下一圈再来),**不重试、不忙等**。
  // 返回这一轮真的交付出去的字节数。
  template <typename Sink>
  size_t drain(size_t budget_bytes, Sink&& sink) {
    if (!ready()) return 0u;
    size_t budget = (budget_bytes < m_len) ? budget_bytes : m_len;
    size_t moved = 0u;
    while (budget > 0u) {
      const size_t chunk = (budget < (m_cap - m_head)) ? budget : (m_cap - m_head);
      const size_t took = sink(m_buf + m_head, chunk);
      if (took == 0u) {
        ++m_blocked_drains;  // 可观测:"排空被端口挡住过几次"
        break;
      }
      const size_t done = (took < chunk) ? took : chunk;
      m_head = (m_head + done) % m_cap;
      m_len -= done;
      budget -= done;
      moved += done;
      if (done < chunk) {  // 端口只吃下一部分 ⇒ 它也满了,这一圈到此为止
        ++m_blocked_drains;
        break;
      }
    }
    m_drained_bytes += moved;
    return moved;
  }

  // 丢掉环里**全部**内容(返回丢掉的字节数)。给"清屏/重来"这类显式动作留的口子;
  // 主循环里**不该**调它 —— 正常路径上让 drain 慢慢排。
  size_t clear() {
    const size_t n = m_len;
    m_head = 0u;
    m_len = 0u;
    return n;
  }

  size_t droppedBytes() const { return m_dropped_bytes; }
  size_t dropCount() const { return m_drop_count; }
  size_t writtenBytes() const { return m_written_bytes; }
  size_t drainedBytes() const { return m_drained_bytes; }
  size_t blockedDrains() const { return m_blocked_drains; }
  size_t highWaterMark() const { return m_hwm; }

 private:
  uint8_t* m_buf = nullptr;
  size_t m_cap = 0u;
  size_t m_head = 0u;
  size_t m_len = 0u;
  size_t m_dropped_bytes = 0u;
  size_t m_drop_count = 0u;
  size_t m_written_bytes = 0u;
  size_t m_drained_bytes = 0u;
  size_t m_blocked_drains = 0u;
  size_t m_hwm = 0u;
};

// 速率闸门(节流):"到点才放行"。
// ★ 用途只有一个 —— 把**周期性遥测**那几行(`rgb:` / `SRC-…` / `206 dash ok`)
//   降频;**守护 / 蜂鸣器 / 诊断页 / 告警 / 链路状态变化那几行一律不许挂它**
//   (它们的价值就在"变化那一刻",节流掉等于把现场删了)。
// ★ 它是**纯计数**,除了"传进来的时钟"不读任何东西 ⇒ 不会引入新的阻塞。
class RateGate {
 public:
  explicit RateGate(uint32_t interval_ms) : m_interval(interval_ms) {}

  void setInterval(uint32_t interval_ms) { m_interval = interval_ms; }
  uint32_t interval() const { return m_interval; }

  // now 到点了吗(到点**不**自动更新 —— 更新由 take() 显式做,
  // 这样"先判、再决定要不要花代价格式化"与"同一拍共用一个闸门"两种用法都能写)。
  bool due(uint32_t now) const { return (uint32_t)(now - m_last) >= m_interval; }

  // 到点就更新并返回 true(最常用的形状,一行搞定)。
  // ★ **第一次一定放行**(`m_started`):`m_last` 从 0 起 ⇒ 上电后 `now` 还很小的时候
  //   用"差值 ≥ 间隔"会把本该出来的那一条挡掉(设备上 `now` 是毫秒,开机那几十毫秒
  //   正好落在这个窗口里;而开机那几行恰恰是最需要看的)。所以第一次显式放行,
  //   之后才交给时钟。
  bool take(uint32_t now) {
    if (!m_started) {
      m_started = true;
      m_last = now;
      return true;
    }
    if (!due(now)) return false;
    m_last = now;
    return true;
  }

  // 值变了就放行(并且顺带把闸门也推到 now,免得"变了"之后紧跟一行重复)。
  // ★ 只吃整数 —— 字符串不进这里(要比字符串就自己比,然后调 take())。
  template <typename T>
  bool changed(uint32_t now, T value) {
    const unsigned long long v = static_cast<unsigned long long>(value);
    if (m_has_last && v == m_last_value) return false;
    m_last_value = v;
    m_has_last = true;
    m_started = true;
    m_last = now;
    return true;
  }

 private:
  uint32_t m_interval;
  uint32_t m_last = 0u;
  bool m_started = false;
  bool m_has_last = false;
  unsigned long long m_last_value = 0ull;
};

}  // namespace dashlogring

// ============================================================================
//  ③ 设备侧:格式化 → 环 → (按预算、先问能不能写) 排给串口
// ============================================================================
#if defined(ARDUINO)

#include <Arduino.h>
#include <stdarg.h>

namespace dashlog {

// 内部状态:环 + 它那块静态内存。★ 全是 POD,没有构造顺序问题、不碰堆。
struct State {
  dashlogring::Ring ring;
  uint8_t storage[DASHLOG_RING_BYTES];
  bool inited = false;
};
inline State& state() {
  static State s;
  return s;
}

// 惰性初始化:第一次真正用到时才认这块静态内存。
inline void ensure() {
  State& s = state();
  if (!s.inited) {
    s.inited = true;
    s.ring.init(s.storage, sizeof(s.storage));
  }
}

// ① 起串口(行为与以前逐字节相同:按 DASH_LOG_UART0 决定开一个口还是两个口)。
inline void begin(unsigned long baud = 115200) {
  ensure();
  Serial.begin(baud);  // USB-CDC:插原生 USB 口时看这个(日志的唯一出口)
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  // UART0 那一路:只在没被链路占着时开(见上面的 DASH_LOG_UART0)。
  // 经典 ESP32 上 Serial 与 Serial0 是同一个 UART0,所以这条一直按 CDC_ON_BOOT 判。
#if DASH_LOG_UART0
  Serial0.begin(baud);  // UART0:插板载 CH340 口时看这个
#endif
#endif
}

// ② 排空:主循环每圈**末尾**调一次。
//   · `Serial.availableForWrite()`(HWCDC 的 TX 环剩余空间)= "现在写会不会撞超时"。
//     报 0 ⇒ **一个字节都不写**,立刻返回(这正是"永不阻塞"那句话的落点)。
//   · UART0 那一路(只在 `DASH_LOG_UART0` 的构建里存在)**不进环**:
//     它由本函数顺带发一份,同样先问 `availableForWrite()`;报 0 就丢这一份。
//     理由:UART0 那条路只在"有人在 43/44 上收"时才通,而那种场合它的
//     `availableForWrite()` 是可靠的;把它也塞进环会让"环里排的是谁"变糊。
inline size_t drain(size_t budget_bytes = DASHLOG_DRAIN_BUDGET) {
  ensure();
  State& s = state();
  if (!s.ring.ready()) return 0u;

  uint8_t tmp[DASHLOG_MAX_LINE];
  return s.ring.drain(budget_bytes, [&](const uint8_t* p, size_t n) -> size_t {
    const size_t want = (n < sizeof(tmp)) ? n : sizeof(tmp);
    // ★ 先问,再写。`availableForWrite()` 自己也带 100 ms 的信号量超时,
    //   但那要"ISR 一直占着 tx_lock"才会等到 —— 正常的环操作是微秒级。
    const int room = Serial.availableForWrite();
    if (room <= 0) return 0u;  // ← 端口满了:不写、不重试,下一圈再说
    size_t cap = (size_t)room;
    // ★ 再留一点余量:底层是 FreeRTOS 环(RINGBUF_TYPE_BYTEBUF),它的"最大可写"
    //   比 `availableForWrite()` 报的数略小;留 8 B 就够,少写几个字节不影响什么。
    cap = (cap > 8u) ? (cap - 8u) : 0u;
    if (cap == 0u) return 0u;
    const size_t take = (want < cap) ? want : cap;
    for (size_t i = 0; i < take; ++i) tmp[i] = p[i];
#if DASH_LOG_UART0
    {
      const int room0 = Serial0.availableForWrite();
      if (room0 > 0) Serial0.write(tmp, take);  // 有地方才写;没地方就丢这一份
    }
#endif
    return Serial.write(tmp, take);  // HWCDC:空间已确认 ⇒ 不会撞超时
  });
}

// 只读统计(写进 5 秒摘要那一行;`drop=` 是**必须可观测**的那一格)。
struct Stats {
  size_t ring_bytes;        // 此刻环里还欠着多少字节
  size_t ring_capacity;
  size_t drained_bytes;     // 累计交付给串口的字节
  size_t dropped_bytes;     // 累计丢弃的字节(环满)
  size_t drop_count;        // 累计丢弃的**整行**数
  size_t blocked_drains;    // 累计"端口报满、这一圈一个字都没写出去"的次数
  size_t high_water_mark;   // 环最满到过多少字节(占用上界)
};

inline Stats stats() {
  ensure();
  const dashlogring::Ring& r = state().ring;
  Stats s{};
  s.ring_bytes = r.size();
  s.ring_capacity = r.capacity();
  s.drained_bytes = r.drainedBytes();
  s.dropped_bytes = r.droppedBytes();
  s.drop_count = r.dropCount();
  s.blocked_drains = r.blockedDrains();
  s.high_water_mark = r.highWaterMark();
  return s;
}

// 给 native 用例/将来别的消费者用的直通口。
inline dashlogring::Ring& ring() {
  ensure();
  return state().ring;
}

}  // namespace dashlog

// 应用层一直用的那两个名字(保持原样:调用点一个都没改)。
inline void dash_log_begin(unsigned long baud = 115200) { dashlog::begin(baud); }

// ★★ 核心:格式化进环(**唯一**的写入路径)。
// 这里**绝不触碰串口** —— 没有 `Serial.write`、没有 `availableForWrite`、没有 delay。
// 环满 = 整行丢 + 计数(`dash_log_stats()`)。
// ★ 为什么只有这一份格式化:一次 `vsnprintf` + 一次纯内存拷贝,别的什么都不干。
//   (曾经想过"再转发给 dashlog::logf",那会把 va_list 传两次、还会把这一行
//    格式化两遍 —— 直接写在这里最省,也没有 `__attribute__((format))` 失效的坑。)
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
inline void dash_logf(const char* fmt, ...) {
  dashlog::ensure();
  char buf[DASHLOG_MAX_LINE];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  const size_t len = (static_cast<size_t>(n) < sizeof(buf))
                         ? static_cast<size_t>(n)
                         : sizeof(buf) - 1u;  // 截断也要发,别把整行吞掉
  dashlog::state().ring.write(buf, len);  // 满了就丢;计数在环里
}

// 主循环末尾排空(名字刻意短)。
inline size_t dash_log_drain(size_t budget_bytes = DASHLOG_DRAIN_BUDGET) {
  return dashlog::drain(budget_bytes);
}

// 只读统计的应用层别名(`dash_log_stats()` 比 `dashlog::stats()` 好读一点)。
inline dashlog::Stats dash_log_stats() { return dashlog::stats(); }

#else  // 宿主机构建(native / pcpreview):没有串口阻塞这回事,直接写 stdout

#include <stdarg.h>
#include <stdio.h>

namespace dashlog {
// ★ 宿主机构建里也**提供同一套环的接口**,这样 native 用例钉住的正是设备端
//   要跑的那份逻辑(`dashlogring::Ring`),而不是另一份替身。
inline dashlogring::Ring& ring() {
  static dashlogring::Ring r;
  static uint8_t storage[DASHLOG_RING_BYTES];
  static bool inited = false;
  if (!inited) {
    inited = true;
    r.init(storage, sizeof(storage));
  }
  return r;
}

// 与设备侧**同名同形**的统计结构(调用点不用分平台写两遍)。
// ★★ 但这里要说清楚:**宿主机上这些计数恒为 0** —— 因为 pcpreview/native
//   根本没有 USB-CDC、也没有"没人读"这回事(日志直接写 stdout,不会阻塞),
//   所以"进环/排空/丢弃"这三件事在宿主机上一次都没发生。
//   `ring()` 那边是**用来测环本身**的(用例自己往里写),不是运行期日志的路径。
//   ⇒ 于是"丢弃可观测"这条判据**只能在设备上读**(串口 5 秒摘要那一行),
//     这不是缺口:它测的正是设备上那个会堵的端口。
struct Stats {
  size_t ring_bytes;
  size_t ring_capacity;
  size_t drained_bytes;
  size_t dropped_bytes;
  size_t drop_count;
  size_t blocked_drains;
  size_t high_water_mark;
};

inline Stats stats() {
  Stats s{};
  s.ring_capacity = ring().capacity();
  return s;
}
}  // namespace dashlog

inline void dash_log_begin(unsigned long = 115200) {}

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
inline void dash_logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

// 宿主机上"排空"是空操作 —— 没有需要排的东西(直接写 stdout)。
inline size_t dash_log_drain(size_t = DASHLOG_DRAIN_BUDGET) { return 0u; }

// 与设备侧同名:恒为 0 的那份统计(理由见上面 `dashlog::stats()`)。
inline dashlog::Stats dash_log_stats() { return dashlog::stats(); }

#endif

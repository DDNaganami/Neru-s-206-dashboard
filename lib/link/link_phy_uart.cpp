// 双板链路协议 v1 —— 真实 UART 的 LinkPhy 实现（§0/§1）。
//
// 三条"非阻塞"到底靠什么成立（全部写在代码里，别只看文件头）：
//   · write()      → 只拷进 mTxBuf，**碰都不碰 UART** ⇒ 绝不阻塞。
//   · pumpTx()     → 只搬 hwTxRoom() 允许的那些字节（主循环里调，§1.2 ①）。
//   · availableForWrite() → min(hwTxRoom, 环剩余, kTxFifoHeadroom) ⇒ "写这么多不阻塞"。
//   · read()       → 只从 mRxBuf 取；mRxBuf 由 UART 的 RX 任务填 ⇒ 主循环不碰驱动锁。
#include "link_phy_uart.h"

#if LINK_PHY_UART

// ★ 只在 S3 上编。经典 ESP32 上 `Serial0` 这个对象**不存在**（`HardwareSerial.h`
//   只在 `ARDUINO_USB_CDC_ON_BOOT == 1` 时声明它，而经典板没有 CDC）⇒ 本文件在那边
//   会炸出一堆 `'Serial0' was not declared`，看着像"代码写错了"，其实只是"这块板不是
//   双板架构的板子"（契约 §0 的 43/44 是 S3 的 UART0 脚）。所以在这里给一条**说得清
//   原因**的编译期错误，别让下一个人去猜。
#if defined(ARDUINO_ARCH_ESP32) && !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "link_phy_uart 只在 ESP32-S3 上编(契约 §0 的 43/44 是 S3 的 UART0 脚;经典 ESP32 没有 Serial0,也不是双板架构的板子)。请去掉该 env 的 -DLINK_PHY_UART。"
#endif

// ★ 这两个头放在**一起**是这道闸门的前提：`dash_log.h` 就是"日志要不要写 UART0"那件事
//   的唯一出处（`DASH_LOG_UART0`，见那个文件头）。
#include "dash_log.h"

// ============================================================
// ★★ 编译期闸门：**链路 UART 与日志 UART 必须是两个不同外设**（§0「载体」的待办）
//
// 为什么闸门在这儿：本翻译单元是"链路 PHY 进了这份固件"的**唯一证据**
// （LDF 只把被引用到的库源码编进来）。所以"链路占 UART0 + 日志也写 UART0"这个危险
// 组合在这里判**最准**：既不会误判（宿主机用例 include link_phy_pins.h 不会碰它），
// 也不会漏（谁把 LinkPhyUart 链进固件，谁就走这一行）。
//
// 契约原话（§0「载体」）：链路用的就是 UART0 那一对脚（43/44）⇒ **显示构建必须关掉
// UART0 文本日志**，日志只走原生 USB-CDC，否则日志文本会混进链路数据流
// （从板会拿它当 VAN 回放行去解，见 §2 的分流）。
//
// ★★ 2026-09-23：**§0 那条待办已经做掉了** —— `dash_log.h` 新增 `DASH_LOG_UART0` 这个
//   编译期开关（有 `LINK_PHY_UART` 时默认 0 ⇒ UART0 一个字节都不写，日志只走 USB-CDC）。
//   于是：
//     · **显示/链路构建不再需要任何豁免宏**（旧写法是在 platformio.ini 里写
//       `-DLINK_PHY_UART_ALLOW_LOG_ON_UART0=1` 承认现状），本条判据直接成立；
//     · 而这道闸门**留着、而且是常开的** —— 它现在挡的是另一件事：谁要是在 env 里
//       显式 `-DDASH_LOG_UART0=1` 把 UART0 日志打开（连同 `LINK_PHY_UART`），
//       那就是"日志与链路共用一个外设"这个危险组合本身 ⇒ 编译期直接拦住。
//   ⇒ 正常构建里这条**永远不会触发**；它触发就说明有人主动把日志又放回了链路那根线上。
//
// ★ 为什么用 `#error` 而不是 `static_assert`（2026-09-23 实测踩过）：
//   一开始写的是"套一层模板的 static_assert + 显式实例化"，结果**它不报**（GCC 对
//   显式实例化里的 static_assert 在 -Os 下的求值时机与预期不同，实测三种写法都没拦住）。
//   这种闸门唯一的价值就是"一定拦得住"，所以换成最朴素、最没有解释余地的 `#error`。
//   代价：报错信息里看不到那段中文长说明（`#error` 的文本会原样打出来，见下面）。
// ============================================================
#if (LINK_UART_PORT == 0) && (DASH_LOG_UART0)
#error "link_phy_uart: 链路占了 UART0(契约 §0 的 43/44),而这份固件又把日志也放在 UART0 上(DASH_LOG_UART0=1) —— 日志文本会混进链路数据流,从板会拿它当 VAN 回放行去解(§0「载体」那条待办的原始风险). 正常构建不会走到这里: dash_log.h 在定义了 LINK_PHY_UART 时默认 DASH_LOG_UART0=0(UART0 不写日志,只走原生 USB-CDC). 两条出路择一: (1) 去掉那个显式覆盖(推荐,就是默认行为); (2) 确认链路不在 UART0 上(-DLINK_UART_PORT=1/2,注意别与 OBD 的 UART1 抢)."
#endif

namespace dashlink {

void LinkPhyUart::begin(bool loopback) {
  // 说明：端口号在编译期由 LINK_UART_PORT 定死（0 = UART0，见契约 §0），这里只选
  // 引脚组。★ 别把这里改成"运行期换端口"：那会让"日志与链路是不是同一个外设"
  // 这件事从编译期退化成运行期，而它正是 §0 那条待办的核心。
  if (loopback) {
    start(&Serial1, kLoopPort, kLoopTxPin, kLoopRxPin, true);
  } else if (kDefaultPort == 0) {
    start(&Serial0, kDefaultPort, kDefaultTxPin, kDefaultRxPin, false);
  } else if (kDefaultPort == 1) {
    start(&Serial1, kDefaultPort, kDefaultTxPin, kDefaultRxPin, false);
#if SOC_UART_NUM > 2
  } else if (kDefaultPort == 2) {
    start(&Serial2, kDefaultPort, kDefaultTxPin, kDefaultRxPin, false);
#endif
  }
}

void LinkPhyUart::start(HardwareSerial* s, int8_t port, int8_t tx, int8_t rx, bool loopback) {
  mSerial = s;
  mPort = port;
  mTxPin = tx;
  mRxPin = rx;
  mLoopback = loopback;
  mTxHead = mTxCount = 0;
  mRxHead = mRxTail = 0;
  mTxTotal = mTxOverflow = 0;
  mRxTotal = mRxOverflow = 0;

  // ★ 显式给引脚（不走板级默认）：契约 §0 要的是 43/44，而"哪个脚"是我们这边定的。
  //   115200 8N1 = §1.1。
  //   ★ 这里与日志口的关系：`dash_log_begin()` 在链路构建里**不再碰 UART0**
  //     （`DASH_LOG_UART0=0`，见文件头那道闸门）—— 所以这一行是 UART0 上唯一的占用者。
  s->begin(kLinkBaud, SERIAL_8N1, rx, tx);

  // 入方向：把环读出来（一次 read(len) 拷一批）。UART0 的驱动环默认 256 B，
  // 与我们的 mRxBuf 一样大 —— 主循环一圈来一次就不会溢。
  mSerial->onReceive([this]() {
    if (mSerial == nullptr) return;                 // 防御：还没 begin
    const int n = mSerial->available();
    if (n <= 0) return;
    // 用 read(buf,len) 而不是逐字节：它一次拿到已经解好的字节，回调里更短。
    uint8_t tmp[64];
    int left = n;
    while (left > 0) {
      const int chunk = left > (int)sizeof(tmp) ? (int)sizeof(tmp) : left;
      const int Got = (int)mSerial->read(tmp, (size_t)chunk);
      if (Got <= 0) break;
      for (int i = 0; i < Got; ++i) {
        const uint16_t next = (uint16_t)((mRxHead + 1u) % kRxRingBytes);
        if (next == mRxTail) {
          // 环满：丢**新来的**字节。丢字节会让对端那一帧 CRC 不过而被重同步掉，
          // 症状是 crc_err 涨 —— 这是链路拥塞最轻的一种坏法（比覆盖旧数据好：
          // 旧数据里可能有半帧，覆盖它只会把两帧都毁掉）。
          ++mRxOverflow;
          continue;
        }
        mRxBuf[mRxHead] = tmp[i];
        mRxHead = next;
        ++mRxTotal;
      }
      left -= Got;
    }
  });
  mOnline = true;
}

int LinkPhyUart::available() {
  if (!mOnline) return 0;
  // ★ 只报**已经进环**的字节数，故意**不**把 UART 驱动环里那些也算进来：
  //   这样"available() == 0"就能让 LinkRx::poll 干净地退出，而把"驱动环 → 本环"
  //   这件事交给 RX 任务（它由驱动在收到字节时调度，不占主循环）。
  return (int)((mRxHead + kRxRingBytes - mRxTail) % kRxRingBytes);
}

int LinkPhyUart::read() {
  if (!mOnline) return -1;
  if (mRxTail == mRxHead) return -1;          // 现在没有（非阻塞契约：不是错误）
  const uint8_t b = mRxBuf[mRxTail];
  mRxTail = (uint16_t)((mRxTail + 1u) % kRxRingBytes);
  return (int)b;
}

int LinkPhyUart::hwTxRoom() const {
  if (mSerial == nullptr) return 0;
  // 依据（见 link_phy_uart.h 文件头）：默认 tx ring buffer = 0 ⇒ 这个数就是
  // **FIFO 空闲格数（128）**，也是"再写多少就开始阻塞"的边界。
  return mSerial->availableForWrite();
}

int LinkPhyUart::availableForWrite() {
  if (!mOnline) return 0;
  int room = hwTxRoom();
  if (room <= 0) return 0;                    // FIFO 满了：不写（绝不等待）
  // ① 留余量：返回 N ⇒ 写 N 个不会撞上 uart_write_bytes 的阻塞路径。
  if (room > (int)kTxFifoHeadroom) room -= (int)kTxFifoHeadroom;
  else return 0;
  // ② 自己的环还有多少位置
  const int ringRoom = (int)(kTxRingBytes - mTxCount);
  if (room > ringRoom) room = ringRoom;
  return room > 0 ? room : 0;
}

size_t LinkPhyUart::write(const uint8_t* data, size_t n) {
  if (!mOnline || data == nullptr || n == 0u) return 0u;
  // ★ 这里**不碰 UART**（§1.2 ①②）：只往环里拷。给多了就少收，剩下的算丢。
  size_t accepted = 0;
  while (accepted < n && mTxCount < kTxRingBytes) {
    mTxBuf[(mTxHead + mTxCount) % kTxRingBytes] = data[accepted];
    ++mTxCount;
    ++accepted;
  }
  if (accepted < n) mTxOverflow += (uint32_t)(n - accepted);
  return accepted;
}

uint16_t LinkPhyUart::pumpTx() {
  if (!mOnline || mSerial == nullptr || mTxCount == 0u) return 0u;
  int room = hwTxRoom();
  if (room > (int)kTxFifoHeadroom) room -= (int)kTxFifoHeadroom;
  else return 0u;                             // FIFO 满：这一圈不写（下一圈再来）
  if (room <= 0 || mTxCount == 0u) return 0u;

  uint16_t want = (uint16_t)((size_t)room < (size_t)mTxCount ? (size_t)room : (size_t)mTxCount);
  uint16_t wrote = 0;
  uint16_t first = (uint16_t)(kTxRingBytes - mTxHead);
  if (first > want) first = want;
  mSerial->write(mTxBuf + mTxHead, (size_t)first);
  wrote = first;
  if (wrote < want) {                         // 环绕回：再搬剩下那段
    const uint16_t rest = (uint16_t)(want - wrote);
    mSerial->write(mTxBuf, (size_t)rest);
    wrote = (uint16_t)(wrote + rest);
  }
  mTxHead = (uint16_t)((mTxHead + wrote) % kTxRingBytes);
  mTxCount = (uint16_t)(mTxCount - wrote);
  mTxTotal += wrote;
  return wrote;
}

}  // namespace dashlink

#endif  // LINK_PHY_UART

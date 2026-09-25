#pragma once
// 设备日志:**默认同时**打到 USB-CDC(原生 USB 口)和 UART0(板载 CH340 那个 UART 口);
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
//   所以两个都发:插哪个口都看得见。车上是两个都没人收,代价只是每次多几百微秒。
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
//       没有链路 PHY ⇒ `DASH_LOG_UART0 = 1` —— **一字未变**的老行为(两个口都发);
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
#ifndef DASH_LOG_UART0
#if LINK_PHY_UART
#define DASH_LOG_UART0 0
#else
#define DASH_LOG_UART0 1
#endif
#endif

#if defined(ARDUINO)

#include <Arduino.h>
#include <stdarg.h>

namespace dashlog {
// 一次格式化、两个口各写一遍 —— ★ 带链路 PHY 的构建里只有**一个**口(见 DASH_LOG_UART0),
// 但还是**只格式化一次**:省一半开销,更重要的是**两个口拿到逐字节相同的内容**
// (排障时两边对不上最费时间)。
constexpr size_t kMaxLine = 320;
}  // namespace dashlog

inline void dash_log_begin(unsigned long baud = 115200) {
  Serial.begin(baud);  // USB-CDC:插原生 USB 口时看这个(日志的唯一出口)
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  // UART0 那一路:只在没被链路占着时开(见上面的 DASH_LOG_UART0)。
  // 经典 ESP32 上 Serial 与 Serial0 是同一个 UART0,所以这条一直按 CDC_ON_BOOT 判。
#if DASH_LOG_UART0
  Serial0.begin(baud);  // UART0:插板载 CH340 口时看这个
#endif
#endif
}

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
inline void dash_logf(const char* fmt, ...) {
  char buf[dashlog::kMaxLine];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  const size_t len = (static_cast<size_t>(n) < sizeof(buf))
                         ? static_cast<size_t>(n)
                         : sizeof(buf) - 1u;  // 截断也要发,别把整行吞掉
  Serial.write(reinterpret_cast<const uint8_t*>(buf), len);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  // ★ 这一行就是"日志混进链路数据流"的唯一入口(UART0 = 契约 §0 的 43/44)
  //   ⇒ 由 DASH_LOG_UART0 收口,见文件头。
#if DASH_LOG_UART0
  Serial0.write(reinterpret_cast<const uint8_t*>(buf), len);
#endif
#endif
}

#else  // 宿主机(pcpreview):没有 Serial 对象,直接写 stdout

#include <stdarg.h>
#include <stdio.h>

inline void dash_log_begin(unsigned long = 115200) {}
inline void dash_logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
}

#endif

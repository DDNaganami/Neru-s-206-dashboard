#pragma once
// 设备日志:**同时**打到 USB-CDC(原生 USB 口)和 UART0(板载 CH340 那个 UART 口)。
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

#if defined(ARDUINO)

#include <Arduino.h>
#include <stdarg.h>

namespace dashlog {
// 一次格式化、两个口各写一遍。比"每个口各格式化一次"省一半开销,
// 更重要的是**保证两个口拿到逐字节相同的内容** —— 排障时两边对不上最费时间。
constexpr size_t kMaxLine = 320;
}  // namespace dashlog

inline void dash_log_begin(unsigned long baud = 115200) {
  Serial.begin(baud);  // USB-CDC:插原生 USB 口时看这个
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  Serial0.begin(baud);  // UART0:插板载 CH340 口时看这个(排障主力)
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
  Serial0.write(reinterpret_cast<const uint8_t*>(buf), len);
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

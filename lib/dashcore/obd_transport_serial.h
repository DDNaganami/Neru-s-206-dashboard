#pragma once
#include "obd_transport.h"

#if defined(ARDUINO)
#include <Arduino.h>
#endif

// ============================================================================
// ObdTransportSerial —— 把 `ObdTransport` 接到一个 `HardwareSerial` 上
// ============================================================================
//
// 这是**原来那条路**（有线 ELM327 接 `Serial1`），行为与抽取之前逐字节一致：
//   · `print(cmd)` → `s->print(cmd)`
//   · `print(pid, HEX)` → 手写两位十六进制（见 `ObdTransport::writeHex`）
//   · `available()` / `read()` 直通
//
// ★ 为什么还留着它：2.8C 上 UART 版用不了（引脚被 RGB 并口占了），但
//   经典 ESP32 / 台架 / 以后换板子都可能再用；而且它是"串口那半边行为没变"
//   的**证据** —— 抽传输层时最怕的就是顺手改了原有行为，有它在就能对照。
//
// ★ `begin` 由调用方在构造后自己调（波特率/引脚因板而异，而且必须在 `setup()`
//   里做，不能在静态初始化期做 —— 理由见 `src/main.cpp` 里那段注释）。
#if defined(ARDUINO)
class ObdTransportSerial : public ObdTransport {
public:
  explicit ObdTransportSerial(HardwareSerial* s = nullptr) : s_(s) {}

  bool start() override { return s_ != nullptr; }
  bool connected() const override { return s_ != nullptr; }

  void write(const char* str) override { if (s_) s_->print(str); }
  void write(char c) override { if (s_) s_->print(c); }

  int available() override { return s_ ? s_->available() : 0; }
  int read() override { return s_ ? s_->read() : -1; }

private:
  HardwareSerial* s_;
};
#endif  // ARDUINO

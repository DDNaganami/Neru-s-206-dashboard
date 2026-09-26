#pragma once
#include "obd_transport.h"
#include "test_helpers.h"   // FakeSerial（桥接用；这个头本身不动它）
#include <deque>
#include <string>

// ============================================================================
// FakeObdTransport —— 实现 `ObdTransport` 的假传输（记录 TX、按需回放 RX）
// ============================================================================
//
// 为什么单独一个文件、不去改 `test_helpers.h` 里那个 `FakeSerial`：
//   `FakeSerial` 继承的是 `HardwareSerial`，是**既有那批用例**的基础设施。
//   2026-09-27 实测：把它的基类换成 `ObdTransport` 之后，清缓存重建会得到
//   `van::crc15_van_iso` / `van::parseFrameBytes` / `Alerts::update` 一片
//   undefined reference —— 这个 native 构建的库扫描对 `test_helpers.h` 很敏感。
//   ⇒ **别动它**；新东西另起一个头，两条路各自独立。
//
// ★ 它还承担一件原来那个假串口做不到的事：模拟 **BLE 的分片到达**
//   （`feedChunked()`）。实测诊断头把 `ATZ` 的回答切成 1+13 两片送出来，
//   而"必须按 `>` 收尾、不能假设一次读=一整条回答"正是要钉的纪律。
class FakeObdTransport : public ObdTransport {
public:
  std::string tx;
  std::deque<char> rx;

  // --- ObdTransport ---
  bool start() override { ++starts; return start_ok; }
  bool connected() const override { return conn; }

  void write(const char* s) override { tx += s; }
  void write(char c) override { tx += c; }

  int available() override { return (int)rx.size(); }
  int read() override {
    if (rx.empty()) return -1;
    const char c = rx.front();
    rx.pop_front();
    return (uint8_t)c;
  }

  // --- 测试用 ---
  bool start_ok = true;   // start() 返回什么（验"链路用不了"那条路）
  bool conn = true;       // connected() 报什么（BLE 断开）
  int  starts = 0;

  void feed(const char* s) {
    while (*s) rx.push_back(*s++);
  }
  // 分片投喂：把 `s` 按 `chunk` 字节切段推进队列
  void feedChunked(const char* s, size_t chunk) {
    const size_t n = strlen(s);
    for (size_t i = 0; i < n; i += chunk) {
      for (size_t k = i; k < n && k < i + chunk; ++k) rx.push_back(s[k]);
    }
  }
  bool sent(const char* s) const { return tx.find(s) != std::string::npos; }
  void clearTx() { tx.clear(); }
};

// ============================================================================
// FakeSerialAsTransport —— 把既有的假串口 `FakeSerial` 包成一条 `ObdTransport`
// ============================================================================
//
// 为什么需要这座桥：`ObdSource` / `VehicleDataService` 的构造参数已经从
// `HardwareSerial*` 换成了 `ObdTransport*`（见 `obd_transport.h`），
// 而既有的 `test_obd_source.cpp` / `test_data_service.cpp` 用的是 `FakeSerial`
// —— 那个类是**不能动**的（见本文件开头那段实测记录）。
// 于是就地包一层：`ObdSource` 照样拿到 `ObdTransport`，而底下喂的仍是
// 那个用了很久、断言字符串一字不改的 `FakeSerial`。
//
// ★ 行为等价性：包装后的 `write/writeHex/available/read` 与
//   `ObdTransportSerial` 走的是同一套映射（前者 `print(s)`、后者 `print(pid,HEX)`
//   → `writeHex`），所以既有断言的 TX 字符串**逐字节不变**。
class FakeSerialAsTransport : public ObdTransport {
public:
  explicit FakeSerialAsTransport(FakeSerial* f) : f_(f) {}
  bool start() override { return f_ != nullptr; }
  bool connected() const override { return f_ != nullptr; }
  void write(const char* s) override { if (f_) f_->print(s); }
  void write(char c) override { if (f_) f_->print(c); }
  int available() override { return f_ ? f_->available() : 0; }
  int read() override { return f_ ? f_->read() : -1; }

private:
  FakeSerial* f_;
};


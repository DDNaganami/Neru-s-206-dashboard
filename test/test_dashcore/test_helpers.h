#pragma once
#include "Arduino.h"
#include <deque>
#include <string>

// 可编程假串口:记录 TX,按需回放 RX。
//
// ★ 2026-09-27:这个类**故意保持继承 `HardwareSerial` 不变** —— 它是既有那批
//   用例的基础设施,动它会把整个 native 构建的库扫描碰坏(实测:把它改成实现
//   `ObdTransport` 之后,清缓存重建会得到 `van::` 一片 undefined reference)。
//   新代码要的是 `ObdTransport`,对应实现放在同目录的 `fake_obd_transport.h` 里,
//   两者互不干扰。
class FakeSerial : public HardwareSerial {
public:
  std::string tx;
  std::deque<char> rx;

  size_t print(const char* s) override {
    tx += s;
    return strlen(s);
  }
  size_t print(char c) override {
    tx += c;
    return 1;
  }
  size_t print(uint8_t b, int base) override {
    char buf[16];
    if (base == HEX) snprintf(buf, sizeof(buf), "%X", (unsigned)b);
    else snprintf(buf, sizeof(buf), "%u", (unsigned)b);
    tx += buf;
    return strlen(buf);
  }
  int available() override { return (int)rx.size(); }
  int read() override {
    if (rx.empty()) return -1;
    const char c = rx.front();
    rx.pop_front();
    return (uint8_t)c;
  }

  void feed(const char* s) {
    while (*s) rx.push_back(*s++);
  }
  bool sent(const char* s) const { return tx.find(s) != std::string::npos; }
};

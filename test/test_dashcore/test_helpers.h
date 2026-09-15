#pragma once
#include "Arduino.h"
#include <deque>
#include <string>

// 可编程假串口:记录 TX,按需回放 RX。
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

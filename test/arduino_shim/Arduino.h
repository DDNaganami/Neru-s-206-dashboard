#pragma once
// 宿主机单元测试用的 Arduino API 最小桩。
// 仅覆盖 lib/dashcore 用到的符号;不参与固件编译。
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#define HEX 16

class HardwareSerial {
public:
  virtual ~HardwareSerial() = default;
  virtual size_t print(const char* s) { (void)s; return 0; }
  virtual size_t print(char c) { (void)c; return 0; }
  virtual size_t print(uint8_t b, int base) { (void)b; (void)base; return 0; }
  virtual int available() { return 0; }
  virtual int read() { return -1; }
};

uint32_t millis();
void test_set_millis(uint32_t ms);

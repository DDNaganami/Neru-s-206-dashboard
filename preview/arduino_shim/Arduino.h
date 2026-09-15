#pragma once
// 宿主机预览(env:pcpreview)用的 Arduino API 最小桩。
// 和 test/arduino_shim 类似,但多补了 Serial/printf/millis/delay/main,
// 让设备上同一份 main.cpp 原样跑在 PC。
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define HEX 16

class HardwareSerial {
public:
  virtual ~HardwareSerial() = default;
  virtual size_t print(const char* s) { (void)s; return 0; }
  virtual size_t print(char c) { (void)c; return 0; }
  virtual size_t print(uint8_t b, int base) { (void)b; (void)base; return 0; }
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual void begin(unsigned long baud) { (void)baud; }

  // 设备代码用的便捷输出,转发到标准输出
  size_t printf(const char* fmt, ...);
  size_t println(const char* s);
  size_t println();
};

extern HardwareSerial Serial;

uint32_t millis();          // 开机以来毫秒(单调)
void delay(uint32_t ms);

void setup();
void loop();

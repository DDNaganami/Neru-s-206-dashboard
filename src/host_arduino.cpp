// 宿主机预览(env:pcpreview)的 Arduino API 实现。
// 设备构建(esp32dev)定义了 ARDUINO,本文件整个编译为空;
// 预览构建没有 ARDUINO,由本文件提供 millis/delay/Serial/main。
#if !defined(ARDUINO)

#include "Arduino.h"   // preview/arduino_shim/Arduino.h
#include <stdarg.h>
#include <windows.h>

HardwareSerial Serial;

uint32_t millis() {
  static ULONGLONG start = 0;
  if (!start) start = GetTickCount64();
  return (uint32_t)(GetTickCount64() - start);
}

void delay(uint32_t ms) { Sleep(ms); }

size_t HardwareSerial::printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
  return (size_t)(n > 0 ? n : 0);
}

size_t HardwareSerial::println(const char* s) {
  printf("%s\n", s);
  fflush(stdout);
  return strlen(s) + 1;
}

size_t HardwareSerial::println() {
  printf("\n");
  fflush(stdout);
  return 1;
}

int main() {
  setup();
  for (;;) loop();
}

#endif  // !ARDUINO

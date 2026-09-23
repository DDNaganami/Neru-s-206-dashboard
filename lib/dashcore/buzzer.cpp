#include "buzzer.h"
#include <stdio.h>   // snprintf:宿主机与设备都有(设备侧走 newlib)

#if defined(ARDUINO)
#include <Arduino.h>
#include "dash_log.h"   // 日志默认打 USB-CDC+UART0;带链路 PHY 的构建只打 USB-CDC
#endif

#if defined(_WIN32) && defined(BUZZER_HOST_SOUND)
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// 宿主机侧的蜂鸣器：**落一行文本**（默认不发声）
//
// ★ 为什么是"落一行"而不是"直接 printf 了就算"：
//   这一行是报告与自查要引用的**证据**（"按下去之后串口上出现了哪一行"），
//   所以它必须①格式稳定、②能被**用例直接读到**。于是：
//     · 文本先写进一个 64 字节的静态缓冲（buzzer_host_last_line()），
//     · 再由 buzzer_host_printf 打出去（pcpreview 把它接到 stdout）。
//   用例断言的是那个缓冲（不依赖"printf 有没有被转发"），
//   而 pcpreview 上人看到的就是同一行 —— 两边**看的是同一份字节**。
//
// ★ 格式固定为 `BEEP pattern=<name> ms=<n>`：纯 ASCII（README 里那条纪律：
//   测试输出里出现中文会让 PlatformIO 的转发在 GBK 控制台上抛
//   UnicodeEncodeError，把用例统计打乱），一行一个模式名便于 grep。
// ---------------------------------------------------------------------------
static char g_host_line[64] = {0};

const char* buzzer_host_last_line() { return g_host_line; }

#if !defined(ARDUINO) && !defined(DASH_DISPLAY_PREVIEW)
// 只有 native 测试构建会链接它（pcpreview 那边由 src/dash_display.cpp 提供真实现）
void buzzer_host_printf(const char* line) { fputs(line, stdout); }
#endif

void BuzzerHost::begin() {
#if defined(ARDUINO) && !defined(DASH_DISPLAY_PREVIEW)
  dash_logf("buzzer: host(打印模式,不做真发声)\n");
#endif
}

void BuzzerHost::beep(BeepPattern pattern, uint32_t beep_ms) {
  snprintf(g_host_line, sizeof(g_host_line), "BEEP pattern=%s ms=%u",
           beepPatternName(pattern), (unsigned)beep_ms);

#if defined(ARDUINO) && !defined(DASH_DISPLAY_PREVIEW)
  // 设备上万一挂了这一支（比如抓帧盒上想听一声），走日志
  dash_logf("%s\n", g_host_line);
#else
  // 宿主机（native 测试 / pcpreview）：交给外部打印。
  // ★ 这里**声明**而不是 include：native 与 pcpreview 各有一份实现，
  //   而 lib/dashcore 不该反过来依赖 src/。声明与实现的对账放在
  //   native 用例里（它链接的是下面那个默认实现，能链上就说明签名一致）。
  void buzzer_host_printf(const char* line);
  buzzer_host_printf(g_host_line);
#endif

#if defined(_WIN32) && defined(BUZZER_HOST_SOUND)
  // 只在显式打开时才真出声：开发机上突然响一声对跑构建的人是干扰。
  // 三短/四短在这里只是"多响几声"——**判据（什么时候该响）完全不在这里**，
  // 这一层只负责"把模式落成声音"（见 buzzer.h 的分层口径）。
  const int times = (pattern == BeepPattern::Urgent) ? 4
                  : (pattern == BeepPattern::Triple) ? 3
                  : 1;
  for (int i = 0; i < times; ++i) {
    MessageBeep(pattern == BeepPattern::Long ? MB_ICONHAND : MB_ICONASTERISK);
  }
#else
  (void)pattern;
#endif
}

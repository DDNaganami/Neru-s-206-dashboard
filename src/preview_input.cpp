#include "preview_input.h"

// ============================================================
// pcpreview 的输入注入 —— **IO 那一半**
//
// ★ 纯函数（`preview_apply_key` / `preview_apply_control_text`）已经搬到
//   `lib/dashcore/preview_input.h` 里做成 inline 了。为什么：
//     · native 用例要测**同一份**语义，而 native 的 `lib_archive = no` 构建
//       **只编 lib/ 下的源码**（不编 src/）⇒ 纯函数留在本文件里，
//       用例就会拿到两个未定义符号（这是实测踩到的：
//       `undefined symbol: preview_apply_key(PreviewInput&, PreviewKey)`）。
//     · 搬进头文件之后，pcpreview 与 native 用的是逐字节同一份实现，
//       而本文件只剩"读终端 / 读文件"这点事 —— 它本来也没法在宿主机上测。
//
// ★ 本文件**只**编进 `env:pcpreview`：它整体在 `DASH_DISPLAY_PREVIEW` 里
//   （判据与 src/dash_display.cpp 的预览驱动同一套，platformio.ini 的
//   [env:pcpreview] 只加这一个 -D）。于是"设备固件里有没有偷偷读一个 PC 上的
//   文件"这件事在结构上不存在 —— 那三个固件目标里连这个 .cpp 都不编。
//
// ★ 它**不得不依赖 Windows 控制台 API**（非阻塞按键），所以下面按平台分支：
//   非 Windows 上"键盘那一半"自动退化成"只有控制文件那一半"
//   （文件是真的可移植，读文件只用 stdio）。
// ============================================================

#if defined(DASH_DISPLAY_PREVIEW)

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static bool g_began = false;
static char g_ctl_path[256] = {0};

// 控制文件的读取上限（字节）。见 preview_input_poll 里那段"为什么是 1024"。
// ★ 与 lib/dashcore/preview_input.h 的文档是同一口径，改这里要同步改文档。
static const size_t kCtlMax = 1024;

#if defined(_WIN32)
static HANDLE g_hin = INVALID_HANDLE_VALUE;
static DWORD g_old_mode = 0;

// 非阻塞地取一个键；没有键返回 PreviewKey::None。
// ★ 用 `ReadConsoleInput` 而不是 `_kbhit` + `_getch`：前者连**方向键**这种
//   "扩展键"一起给出来（`_getch` 要处理两次调用的 0xE0 前缀，容易漏掉一次，
//   于是"按左键没反应"这种最难查的现象就出现了）。
static PreviewKey win_read_key() {
  if (g_hin == INVALID_HANDLE_VALUE) return PreviewKey::None;
  DWORD n = 0;
  if (!GetNumberOfConsoleInputEvents(g_hin, &n) || n == 0) return PreviewKey::None;
  INPUT_RECORD rec;
  DWORD got = 0;
  if (!ReadConsoleInputA(g_hin, &rec, 1, &got) || got == 0) return PreviewKey::None;
  if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) return PreviewKey::None;
  const WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
  const char ch = rec.Event.KeyEvent.uChar.AsciiChar;
  switch (vk) {
    case VK_LEFT:   return PreviewKey::Left;
    case VK_RIGHT:  return PreviewKey::Right;
    case VK_ESCAPE: return PreviewKey::Clear;
    default: break;
  }
  switch (ch) {
    case ' ':           return PreviewKey::Hazard;
    case 'l': case 'L': return PreviewKey::LowBeam;
    case 'p': case 'P': return PreviewKey::PositionLamp;
    case 'd': case 'D': return PreviewKey::Door;
    case 'o': case 'O': return PreviewKey::Overspeed;
    case 'r': case 'R': return PreviewKey::Redline;
    case 'm': case 'M': return PreviewKey::Mute;
    case 'x': case 'X': return PreviewKey::Clear;
    default:            return PreviewKey::None;
  }
}
#endif  // _WIN32

void preview_input_begin(const char* ctl_path) {
  if (g_began) return;
  g_began = true;
  snprintf(g_ctl_path, sizeof(g_ctl_path), "%s",
           (ctl_path && *ctl_path) ? ctl_path : "preview/inject.txt");
#if defined(_WIN32)
  g_hin = GetStdHandle(STD_INPUT_HANDLE);
  if (g_hin != INVALID_HANDLE_VALUE && GetConsoleMode(g_hin, &g_old_mode)) {
    // 关掉行缓冲与回显：否则按键要等回车才到、而且会回显到控制台里
    // （★ ENABLE_EXTENDED_FLAGS 必须留着，不然方向键这种扩展键收不到）
    SetConsoleMode(g_hin, (g_old_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) |
                              ENABLE_EXTENDED_FLAGS);
  }
#endif
  // 两行回执：告诉用户"键盘能用、控制文件在哪"。
  // ★ 纯 ASCII —— README 那条纪律：预览/测试输出里的中文会在 GBK 控制台上
  //   抛 UnicodeEncodeError，把统计打乱（这条只有踩过才知道）。
  printf("preview input: keys <- -> [space] L P D O R M X/Esc\n");
  printf("preview input: control file = %s\n", g_ctl_path);
}

bool preview_input_poll(PreviewInput& in) {
  const PreviewInput before = in;
  bool handled = false;

  // ① 键盘：一次把攒下的键全吃掉（同一帧连按多次 = 最后一次生效）
#if defined(_WIN32)
  for (int i = 0; i < 16; ++i) {
    const PreviewKey k = win_read_key();
    if (k == PreviewKey::None) break;
    if (preview_apply_key(in, k)) handled = true;
  }
#endif

  // ② 控制文件：每帧读一次（有则读、无则跳过）。文件可以**热改** ——
  //    保存即生效，不需要重启预览。
  //
  // ★ 缓冲 1024 字节，而且**超了会明确报警**（见下）。这两条都是踩出来的：
  //   第一版缓冲只有 512 且对"读满"一言不发，于是"把一个带注释的例子文件
  //   直接存成 inject.txt"就会**静默地全部失效** —— 注释占掉了整个缓冲，
  //   真正那几行 `left=1` 一个字都没读进来，而日志上什么异常都没有
  //   （实测：`parsed=0`、屏上灯全灭，查了半天才想到是缓冲）。
  //   控制文件本来就该是"几行键值"，1024 对它是很宽的量；真有人写超长文件时，
  //   与其静默截断，不如打一行告诉他人话。
  FILE* f = fopen(g_ctl_path, "rb");
  if (f) {
    char buf[kCtlMax + 1];
    const size_t got = fread(buf, 1, kCtlMax + 1, f);
    fclose(f);
    const bool truncated = (got > kCtlMax);
    buf[truncated ? kCtlMax : got] = '\0';
    if (truncated) {
      printf("preview input: WARNING %s is longer than %u bytes; "
             "only the first %u bytes were used\n",
             g_ctl_path, (unsigned)kCtlMax, (unsigned)kCtlMax);
    }
    // ★ 控制文件是"绝对值"：每帧重新施加（不是"和上一帧比变化"）。
    //   于是"把 speed=140 那行删掉"不会把速度退回假数据 —— 要退就写 clear=1
    //   （这一点写在 lib/dashcore/preview_input.h 的用法里，免得当成 bug 查）。
    PreviewInput file_in;
    if (preview_apply_control_text(file_in, buf) > 0) {
      if (file_in.left_set)     { in.left = file_in.left;           in.left_set = true; }
      if (file_in.right_set)    { in.right = file_in.right;         in.right_set = true; }
      if (file_in.hazard_set)   { in.hazard = file_in.hazard;       in.hazard_set = true; }
      if (file_in.low_beam_set) { in.low_beam = file_in.low_beam;   in.low_beam_set = true; }
      if (file_in.position_set) { in.position = file_in.position;   in.position_set = true; }
      if (file_in.door_set)     { in.door = file_in.door;           in.door_set = true; }
      if (file_in.speed_set)    { in.speed_kmh = file_in.speed_kmh; in.speed_set = true; }
      if (file_in.rpm_set)      { in.rpm = file_in.rpm;             in.rpm_set = true; }
      in.mute = file_in.mute;
      handled = true;
    }
  }

  if (!handled && memcmp(&before, &in, sizeof(PreviewInput)) != 0) handled = true;
  return handled;
}

#endif  // DASH_DISPLAY_PREVIEW

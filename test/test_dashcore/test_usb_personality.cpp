// ============================================================
// 板上 USB/串口"人格"的选择（`lib/dashcore/usb_personality.h`）—— 2026-09-26 新增
//
// 这一组回答的是车主现场那句话背后的那一问：
//   "**正常插电（不按 BOOT）也想变成原生 USB**，这样 Type-C 一根线既给电又给日志，
//    43/44 就空出来给链路。"
//
// ★★ 为什么一组用例里**大半在钉"没做的事"**（而这是刻意的）：
//   本单**默认不驱动 GPIO0**（`USB_PERSONALITY_AUTO = 0`），原因是它同时是
//   BOOT strapping 脚 —— 复位期间保持低有可能把芯片带进 ROM 下载模式
//   （纪律照 `ARCHITECTURE.md` §7.5.7："只写方案 + 报 owner"）。
//   ⇒ 于是"这份固件**没有**偷偷去扳那颗开关"本身就是**必须被钉住的交付物**：
//     哪天有人（包括未来的我）顺手把默认值改成 1，这一组会立刻红，
//     提醒他"这一步要 owner 拍板、要贴四条判据"。
//
// ★ 三条判据：
//   ① 极性与脚位是**一处**常量，且只有两个合法值（车主实测：GPIO0 低 = 原生 USB）；
//   ② 默认档 = "一个寄存器都不碰"（`kAutoSelectEnabled == false`）；
//   ③ 日志那一行必须**纯 ASCII** 且两种人格**名字不同**（它要能与设备管理器对上）。
// ============================================================
#include <unity.h>
#include <stdint.h>
#include <string.h>

#include "usb_personality.h"

// ============================================================
// 一、脚位与极性（一处常量，值域钉死）
//   ★ 极性出处是**车主实测**：按住 BOOT（= 把 GPIO0 拉低）插电 ⇒ 枚举成原生 USB。
//     所以"选原生 USB"= 拉低。这一条如果被改反，症状是"插电变成 CH343P 那一边"
//     （屏照跑、日志却跑到 43/44 那条线上）—— 不报错，只是"按 BOOT 才有日志"。
// ============================================================
void test_usb_personality_sel_pin_and_polarity(void) {
  TEST_ASSERT_EQUAL_INT8(0, dashusb::kSwitchSelPin);          // 网络 UART0SEL = IO0
  TEST_ASSERT_EQUAL_UINT8(0u, dashusb::kSelectNativeLevel);   // 低 = 原生 USB（实测）
  // ★ 反向判据：极性只能是 0/1 里的一个（写成别的值等于把"电平"当成了别的东西）
  TEST_ASSERT_TRUE(dashusb::kSelectNativeLevel == 0u ||
                   dashusb::kSelectNativeLevel == 1u);
}

// ============================================================
// 二、★★ 默认档：**不驱动 GPIO0**（本单的交付边界）
//   ★ 这一条红了的含义不是"代码坏了"，而是"有人把默认值打开了" ——
//     那件事要求：owner 拍板 + 上板四条判据（见 usb_personality.h 第五节）。
// ============================================================
void test_usb_personality_default_is_do_not_touch(void) {
#if USB_PERSONALITY_AUTO
  // 打开的那一档：只允许 0/1（编译期已经 #error 拦过一次，这里再钉一次给用例看）
  TEST_ASSERT_TRUE(USB_PERSONALITY_AUTO == 1);
  TEST_ASSERT_TRUE(dashusb::kAutoSelectEnabled);
#else
  TEST_ASSERT_FALSE_MESSAGE(dashusb::kAutoSelectEnabled,
                            "默认档必须是不碰 GPIO0（开了就要贴四条上板判据）");
#endif
}

// ============================================================
// 三、日志那一行：两种人格名字不同、都非空、都纯 ASCII
//   ★ 纯 ASCII 是这台机器上的一条硬纪律（GBK 控制台对中文抛 UnicodeEncodeError，
//     而这一行是开机第一批日志之一）。
//   ★ "两个名字必须不同"的理由：这一行要回答"固件要求哪一边"，
//     两种人格同名的话它就是个恒真的装饰。
// ============================================================
void test_usb_personality_log_name_is_distinct_ascii(void) {
  const char* n = dashusb::requestedPersonalityName();
  TEST_ASSERT_NOT_NULL(n);
  TEST_ASSERT_TRUE(n[0] != '\0');
  for (const char* p = n; *p; ++p) TEST_ASSERT_TRUE((uint8_t)*p < 0x80u);
#if USB_PERSONALITY_AUTO
  TEST_ASSERT_TRUE(strstr(n, "native-usb") != nullptr);
#else
  TEST_ASSERT_TRUE_MESSAGE(strstr(n, "ch343p") != nullptr,
                           "默认档那一边的日志要说清 43/44 归板载桥");
#endif
  // 这一行必须**提到**它自己在说的是哪一根脚（否则读日志的人不知道它碰的是什么）
  TEST_ASSERT_TRUE(strstr(n, "GPIO") != nullptr ||
                   strstr(n, "43/44") != nullptr);
}

void register_usb_personality_tests(void) {
  RUN_TEST(test_usb_personality_sel_pin_and_polarity);
  RUN_TEST(test_usb_personality_default_is_do_not_touch);
  RUN_TEST(test_usb_personality_log_name_is_distinct_ascii);
}

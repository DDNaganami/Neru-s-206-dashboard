// ============================================================
// 串口单字符命令的判据层（2026-09-24 新增）
//
// 这一组回答的是"**按了没反应**"这一类现象里最贵的那一问：
// 串口上那一颗字节到底算不算命令？判据有两半，这里钉的是**命令表那一半**
// （另一半"只认行首 / 别吃回放行"在 `main.cpp` 的 `serial_cmd_handle()` 里，
//   它依赖行缓冲与回放路径 ⇒ 宿主机编不到；见 `serial_cmd.h` 的说明）。
//
// 为什么值得测：
//   · 这个口上**同时**跑着日志与 VAN 回放帧（`VAN 824 18F8271D000000` 这种行里
//     本来就可能含 `d`/`b`/`m`）⇒ 命令表一旦"多认一个字符"，抓帧/回放就会被吃掉；
//   · 而"少认一个字符"的表现是"按了没反应"，在车上几乎没法查。
//   · ★ `r`（面板重初始化）是**现场救黑屏**的唯一软件手段 ⇒ 它必须被认出来；
//     而它一旦**误**认（比如把某个回放字符吃成重初始化），就会给面板来一次
//     不必要的复位 ⇒ 两个方向都要钉住。
// ============================================================
#include <unity.h>
#include <stdint.h>

#include "serial_cmd.h"

// ============================================================
// 一、命令表：**只有这五个小写字符**是命令
// ============================================================
void test_serial_cmd_table(void) {
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Diag,   (uint8_t)serial_cmd_classify('d'));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Mute,   (uint8_t)serial_cmd_classify('m'));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Beep,   (uint8_t)serial_cmd_classify('b'));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Reinit, (uint8_t)serial_cmd_classify('r'));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Inject, (uint8_t)serial_cmd_classify('i'));
  // ★ 注入那一档的**动作**由编译开关决定（默认构建里恒 false ⇒ 调用方把它当普通字符）
  //   —— 判据层只回答"这个字符可能是注入"，行为逐字节变不变由调用方那两行决定。
}

// ============================================================
// 二、大写一律**不是**命令（这一条直接保护回放：`VAN …` 行的首字符是 V）
//    ★ 大写 D/M/B/R 在十六进制里根本不出现，但"多认一个"比"少认一个"危险得多。
// ============================================================
void test_serial_cmd_rejects_uppercase(void) {
  const char ups[] = { 'D', 'M', 'B', 'R', 'I', 'V', 'A', 'N' };
  for (unsigned i = 0; i < sizeof(ups); i++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::None,
                            (uint8_t)serial_cmd_classify(ups[i]));
  }
}

// ============================================================
// 三、回放行里真会出现的那些字符**都不是**命令
//    （`VAN 824 18F8271D000000`：十六进制只用 0-9A-F，小写贴法只用 0-9a-f
//      ⇒ 危险的是 a-f 与数字；这里把 a-f 与数字逐个过一遍）
// ============================================================
void test_serial_cmd_hex_chars_are_not_commands(void) {
  // ★ `b` 与 `d` 是**唯二**落在十六进制字母表里的小写命令字符！
  //   ⇒ 它们能工作**完全**依赖调用方那条"只认行首 / 缓冲里已有 V/v 时不吃"的判据。
  //   这一条断言是**刻意**写出来的：它把"为什么必须要行首判据"钉在案上。
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Beep, (uint8_t)serial_cmd_classify('b'));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Diag, (uint8_t)serial_cmd_classify('d'));
  // 其余十六进制字符全不是命令
  const char hex[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                       'a', 'c', 'e', 'f' };
  for (unsigned i = 0; i < sizeof(hex); i++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::None, (uint8_t)serial_cmd_classify(hex[i]));
  }
}

// ============================================================
// 四、控制字符 / 空白 / 杂散字节都不是命令
//    （`\r`/`\n` 是回放行的结束符；那一颗"上电时 RX 悬空"的杂散字节也在这里）
// ============================================================
void test_serial_cmd_control_chars_are_not_commands(void) {
  const char ctl[] = { '\r', '\n', '\t', ' ', '\0', 'x', 'q', '?', '-', '#' };
  for (unsigned i = 0; i < sizeof(ctl); i++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::None, (uint8_t)serial_cmd_classify(ctl[i]));
  }
}

// ============================================================
// 五、纯函数：同一个字符问两次答案一样（没有隐藏状态）
//    ★ 这条看着像废话，但它钉的是"判据层不许记住任何东西"这条设计
//      （状态全在调用方的行缓冲里，见 serial_cmd.h）。
// ============================================================
void test_serial_cmd_is_pure(void) {
  for (char c = 'a'; c <= 'z'; c++) {
    const uint8_t a = (uint8_t)serial_cmd_classify(c);
    const uint8_t b = (uint8_t)serial_cmd_classify(c);
    TEST_ASSERT_EQUAL_UINT8(a, b);
  }
  // `r` 特别再问一次：它是现场救黑屏的那条命令，答案必须稳定
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Reinit, (uint8_t)serial_cmd_classify('r'));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Reinit, (uint8_t)serial_cmd_classify('r'));
}

// ============================================================
// 六、`i` 的次数位（临时注入路径）：`i1` / `i4` / `i` 各是几次
//    ★ 这一层是纯函数 ⇒ 宿主上可测；"数字本身不是命令"这条也要钉住
//      （否则这个口上回放行里的一颗数字会被当成命令吃掉一个字节）。
// ============================================================
void test_serial_cmd_inject_count(void) {
  // 不跟数字 ⇒ 用默认值（4 次：必然攒够"连续 3 次"）
  TEST_ASSERT_EQUAL_UINT8(4u, serial_cmd_inject_count('\0', 4u));
  TEST_ASSERT_EQUAL_UINT8(4u, serial_cmd_inject_count('\r', 4u));
  TEST_ASSERT_EQUAL_UINT8(4u, serial_cmd_inject_count('x', 4u));
  TEST_ASSERT_EQUAL_UINT8(4u, serial_cmd_inject_count('0', 4u));   // '0' 不在 1..9 里
  // 跟着数字 ⇒ 就是那一位（`i1` 用来量"发现窗"、`i4` 用来造"连续 3 次"）
  TEST_ASSERT_EQUAL_UINT8(1u, serial_cmd_inject_count('1', 4u));
  TEST_ASSERT_EQUAL_UINT8(4u, serial_cmd_inject_count('4', 4u));
  TEST_ASSERT_EQUAL_UINT8(9u, serial_cmd_inject_count('9', 4u));
  // ★ 数字**本身不是命令**（`serial_cmd_classify('3') == None`）：
  //   它只有在"刚认出 `i`"之后才被调用方吃掉一位。
  for (char c = '0'; c <= '9'; c++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::None, (uint8_t)serial_cmd_classify(c));
  }
}

void register_serial_cmd_tests(void) {
  RUN_TEST(test_serial_cmd_table);
  RUN_TEST(test_serial_cmd_rejects_uppercase);
  RUN_TEST(test_serial_cmd_hex_chars_are_not_commands);
  RUN_TEST(test_serial_cmd_control_chars_are_not_commands);
  RUN_TEST(test_serial_cmd_is_pure);
  RUN_TEST(test_serial_cmd_inject_count);
}

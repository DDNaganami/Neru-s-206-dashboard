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
//   · ★★ 2026-10-15：`m`（静音）与 `b`（蜂鸣器试响）随蜂鸣器一起删了
//     （车主原话"蜂鸣器功能砍掉，节省资源"）。**删字符**也是两个方向之一：
//     它现在必须返回 `None`、照旧进回放路径（见 test_serial_cmd_mb_are_not_commands）。
// ============================================================
#include <unity.h>
#include <stdint.h>

#include "serial_cmd.h"

// ============================================================
// 一、命令表：**只有这几个小写字符**是命令
// ============================================================
// ★★ 2026-10-15：原来这里还有 `m`（静音）与 `b`（蜂鸣器试响）两条。蜂鸣器整车
//   砍掉（车主原话"蜂鸣器功能砍掉，节省资源"）⇒ 那两个字符**不再是命令**
//   （见下面 test_serial_cmd_mb_are_not_commands，它把"删掉之后照旧进回放路径"
//   这件事钉住 —— 删字符的方向与"多认一个字符"一样要有人管）。
void test_serial_cmd_table(void) {
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Diag,   (uint8_t)serial_cmd_classify('d'));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Reinit, (uint8_t)serial_cmd_classify('r'));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Inject, (uint8_t)serial_cmd_classify('i'));
  // ★ 注入那一档的**动作**由编译开关决定（默认构建里恒 false ⇒ 调用方把它当普通字符）
  //   —— 判据层只回答"这个字符可能是注入"，行为逐字节变不变由调用方那两行决定。
  // ★★ 2026-09-27 新增 `w` = **RF 测速/测丢包**（下一单要在 10 分钟内量出
  //   丢包/间隔/抖动，现场不可能为了开一次测量重刷固件 ⇒ 敲一个字符就开跑）。
  //   与 `i` 逐字同一个口径：判据层只说"可能是那条命令"，
  //   **默认构建（没有无线 PHY）里调用方返回 false ⇒ 它照旧进回放路径**。
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Wire, (uint8_t)serial_cmd_classify('w'));
}

// ★★ 2026-10-15 新增：**删掉的那两个字符必须是 None**
//   `m` / `b` 以前是命令，现在蜂鸣器砍了 ⇒ 它们必须**干净地**回到"不是命令"。
//   ★ 为什么这条值得单独钉：它们都落在**十六进制字母表**里（a-f 那一段），
//     而这个串口上同时跑着 `VAN 824 18F8271D000000` 这种回放行 ⇒
//     "多认一个字符"会吃掉回放行、"少认一个"才会让命令按了没反应。
//     删字符属于后者，方向安全，但一样要有人管住（免得哪天又被人加回来一半）。
void test_serial_cmd_mb_are_not_commands(void) {
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::None, (uint8_t)serial_cmd_classify('m'));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::None, (uint8_t)serial_cmd_classify('b'));
}

// ============================================================
// 二、大写一律**不是**命令（这一条直接保护回放：`VAN …` 行的首字符是 V）
//    ★ 大写 D/I/M/B 在十六进制里根本不出现，但"多认一个"比"少认一个"危险得多。
//    ★★ 2026-10-15 注意：**`R`（大写）= `SerialCmd::Rebuild`**，它是**命令**，
//      不在这一组里 —— 加它进来会让这条用例红（那正是它该做的：
//      这个数组的语义是"这些大写字母**不是**命令"，写错一个就是写错判据）。
// ============================================================
void test_serial_cmd_rejects_uppercase(void) {
  const char ups[] = { 'D', 'I', 'M', 'B', 'V', 'A', 'N' };
  for (unsigned i = 0; i < sizeof(ups); i++) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::None,
                            (uint8_t)serial_cmd_classify(ups[i]));
  }
  // `R` 是**唯一**大写命令（面板全量重建），单独钉住这个例外
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Rebuild,
                          (uint8_t)serial_cmd_classify('R'));
}

// ============================================================
// 三、回放行里真会出现的那些字符**都不是**命令
//    （`VAN 824 18F8271D000000`：十六进制只用 0-9A-F，小写贴法只用 0-9a-f
//      ⇒ 危险的是 a-f 与数字；这里把 a-f 与数字逐个过一遍）
// ============================================================
void test_serial_cmd_hex_chars_are_not_commands(void) {
  // ★★ 2026-10-15：`b`（试响）原来也是"落在十六进制字母表里的小写命令"之一，
  //   它随蜂鸣器删掉之后，这一档**只剩 `d`** 一个（`b` 现在归下面那批"不是命令"）。
  //   ⇒ 它们能工作**完全**依赖调用方那条"只认行首 / 缓冲里已有 V/v 时不吃"的判据。
  //   这一条断言是**刻意**写出来的：它把"为什么必须要行首判据"钉在案上。
  TEST_ASSERT_EQUAL_UINT8((uint8_t)SerialCmd::Diag, (uint8_t)serial_cmd_classify('d'));
  // 其余十六进制字符全不是命令（`b` 现在也在这批里 —— 蜂鸣器已删）
  const char hex[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                       'a', 'b', 'c', 'e', 'f' };
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
  RUN_TEST(test_serial_cmd_mb_are_not_commands);
  RUN_TEST(test_serial_cmd_rejects_uppercase);
  RUN_TEST(test_serial_cmd_hex_chars_are_not_commands);
  RUN_TEST(test_serial_cmd_control_chars_are_not_commands);
  RUN_TEST(test_serial_cmd_is_pure);
  RUN_TEST(test_serial_cmd_inject_count);
}

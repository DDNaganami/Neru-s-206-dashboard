#pragma once
#include <stdint.h>

// ============================================================
// 串口单字符命令的**判据层**（2026-09-24 新增）
//
// ★ 为什么把这一层抽出来（而不是继续写在 `main.cpp` 的 `serial_cmd_handle` 里）：
//   `main.cpp` 是 `src/` 下的文件，而 native 构建**只编 `lib/`**
//   （`lib_archive = no` + 不给 native 加 `-I src`，见 `platformio.ini` 里那段说明）
//   ⇒ 写在 `main.cpp` 里的判据在宿主机上**根本编不到、测不了**。
//   而这一层恰好是"按了没反应"这类现象的唯一判据（本仓库踩过一次：
//   一颗杂散字节把"行首"占住 ⇒ 后面每条命令都失联）。
//   ⇒ 判据搬进 `lib/dashcore/serial_cmd.h`，`main.cpp` 改调它，两处不再各写一份。
//
// ★★ 这一层**只回答"这个字符是哪条命令"**，不碰任何状态：
//   · "要不要把它当命令"（行首判据 / 回放行保护）在**调用方**——
//     那段与行缓冲、回放路径纠缠在一起，是 `serial_cmd_handle()` 的职责；
//   · "命令做什么"也在调用方（诊断页/静音/响一声/面板重初始化）。
//   ⇒ 这里是一个**纯函数**：同一个字符永远得到同一个答案，没有副作用。
//
// ★ 命令表（**只认小写**；大写一律不是命令 —— 这个口上贴的是回放帧，
//   `VAN`/`van` 那种行必须原样进回放路径）：
//   · `d` ⇒ 诊断页（与预览的 `K` 同一个三步循环）
//   · `m` ⇒ 静音开关取反 + 写 NVS
//   · `b` ⇒ 蜂鸣器测试响一声（人耳判据的入口）
//   · `r` ⇒ **面板重初始化 + 重发当前画面**（2026-09-24 新增，"仪表盘必须常亮"
//            的第 ③ 层：现场不用按物理 RST 也能把黑屏救回来）
//   · `i` ⇒ **临时故障注入**（只在 `-DPANEL_GUARD_FAULT_INJECT=1` 的测试构建里
//            真的有动作；默认构建里调用方会把它当**普通字符**放回回放路径
//            ⇒ 行为逐字节不变，见 `SerialCmd::Inject` 那段说明）
//            ★ 可以跟一位数字表示"注入几次"（`i1` = 一次、`i4` = 四次）；不跟就是 4 次。
//   · `w` ⇒ **RF 测速/测丢包**：开一次 N 帧的编号流（2026-09-27 新增；只在
//            `LINK_PHY_ESP_NOW=1` 的构建里真的有动作，其余构建里恒为"不是命令"
//            ⇒ 行为逐字节不变，判据在 `SerialCmd::Wire` 那段）。
// ============================================================

enum class SerialCmd : uint8_t {
  None = 0,
  Diag,
  Mute,
  Beep,
  Reinit,
  Inject,
  Wire,
};

// `i` 后面那一位数字（"注入几次"）的解析：不是 '1'..'9' 就用 `dflt`。
// ★ 单独一个函数是为了让"`i3` 是几次、`i` 又是几次"这件事在宿主机上可测、可读；
//   数字**本身不是命令**（`serial_cmd_classify('3')` 仍是 `None`），
//   由调用方在认出 `i` 之后自己吃掉下一位。
inline uint8_t serial_cmd_inject_count(char digit, uint8_t dflt) {
  if (digit < '1' || digit > '9') return dflt;
  return (uint8_t)(digit - '0');
}

// 字符 → 命令。★ 认不出来一律 `None`（调用方据此把它交给回放路径）。
inline SerialCmd serial_cmd_classify(char c) {
  switch (c) {
    case 'd': return SerialCmd::Diag;
    case 'm': return SerialCmd::Mute;
    case 'b': return SerialCmd::Beep;
    case 'r': return SerialCmd::Reinit;
    // ★ `i` 的**动作**由调用方按编译开关决定：
    //   `SerialCmd::Inject` 只是"这个字符**有可能**是注入命令"，
    //   而 `fault_inject_from_serial()` 在默认构建里恒为 false ⇒
    //   调用方返回 false ⇒ 这个字符照旧进回放路径（与加这一条之前**完全一样**）。
    case 'i': return SerialCmd::Inject;
    // ★★ `w` = **RF 测速/测丢包**（2026-09-27 新增）—— 与 `i` **逐字同一个口径**：
    //   这里只说"这个字符**有可能**是那条命令"，真正的动作由调用方按编译开关决定
    //   （`LINK_PHY_ESP_NOW`）。**默认构建里它是一个普通字符**（照旧进回放路径）
    //   ⇒ 抓帧盒 / 真屏那些没有无线 PHY 的构建，串口行为**一个字节都没变**。
    //   为什么值得占一个字母：下一单要在 10 分钟内量出"丢包/间隔/抖动"，
    //   而现场不可能为了开一次测量重刷固件 —— 敲一个字符就开跑。
    case 'w': return SerialCmd::Wire;
    default:  return SerialCmd::None;
  }
}

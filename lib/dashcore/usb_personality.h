#pragma once
#include <stdint.h>

// ============================================================
// ★★ 板上 USB/串口"**人格**"的选择（`FSUSB42UMX` 的 `SEL` = GPIO0）—— 2026-09-26 新增
//
// ★★ **本文件默认什么都不做**（`USB_PERSONALITY_AUTO` 默认 0）。
//    它不是"半成品"，而是一次**按 §7.5.7 纪律的交付**：方案 + 风险 + 退回路径都写在
//    这里，而"要不要真的在 setup() 里驱动 GPIO0"由 owner 拍板（理由见第四节）。
//    判据给出之后，把它接到构建里是**一行 -D** 的事。
//
// ------------------------------------------------------------
// 一、现场（车主的实测，2026-09-26，逐字）
// ------------------------------------------------------------
//   · 两块板**都显示为 `USB 串行设备`**（`VID_303A&PID_1001` = **芯片原生 USB**）。
//   · 车主的判据：**按住 BOOT 插电 ⇒ 枚举成原生 USB** ⇒ 说明 `SEL` 的极性是
//     "**GPIO0 拉低 = 选原生 USB**"（拉高/放开 = 选板载 `CH343P`）。
//
//   ★★ **2026-09-27 更正（出处归属，代码行为未改）**：下面这两句里的
//   "`SEL`(10) 由网络 `UART0SEL` 驱动 / 只接 `IO0` + 两个 10 K" ——
//   **那条读自 Touch 版（`ESP32-S3-Touch-LCD-2.8C`）的原理图，而车主的板子是
//   非触控版 `ESP32-S3-LCD-2.8C`**。非触控版的资料页**没有**自己的原理图
//   （它给的那份 PDF 与 Touch 版**逐字节相同**，2026-09-27 核过 SHA256）。
//   本款（非触控）已由「wiki 原话 + 车主台面实测」定案为"**按 UART Type-C
//   插没插**选边"，与 GPIO0 无关 —— 详见 `ARCHITECTURE.md` §8.2 与
//   `docs/LINK-TWO-BOARD.md`「前置条件」那一节。
//   ⇒ 本文件的**常量、默认值、行为一个字都没动**（默认仍是 `AUTO = 0`、
//     一个寄存器都不碰）；这里改的只有**那条出处的归属**。
//     本款其实**不需要**这个开关：只要**不插** Type-C，排针 43/44 就是通的。
//
//   这条与厂商图能对上：`SEL`(10) 由网络 `UART0SEL` 驱动（**Touch 版口径，见上**），
//   而它只接三个端子 —— `IO0`（= GPIO0）+ 两个 10 K（`R44`/`R45`）。
//   ★ 顺带记一条同一张图上的读数不一致：本单用几何抽取读到 `R44` 的阻值文字是
//     **`1K`**（不是 10 K），且它一端接 `VBUS` —— 同样**不构成非触控版的结论**，
//     只作为"将来拿到本款原理图时先核这两处"的线索（`ARCHITECTURE.md` §8.2）。
//   wiki 对这颗开关的描述是
//   *"when the UART Type-C is connected, the 4Pin UART is disabled; when the UART
//   Type-C is not connected, the 4Pin UART is enabled"* —— ★ 这一句才是**本款
//   唯一有出处的**那条（它就在非触控版自己的文档页上）。
//
// ------------------------------------------------------------
// 二、为什么想要它（收益是实的）
// ------------------------------------------------------------
//   现在的处境：43/44 上挂着那颗二选一开关，而**日志要走原生 USB-CDC**
//   （`dash_log.h` 的 `DASH_LOG_UART0 = 0`）。可插电默认选的是 CH343P 那一边
//   ⇒ 台面上"要日志就得按住 BOOT 插电"。如果固件自己把开关扳到原生 USB：
//     · Type-C 给电 + 给日志（一根线到底）；
//     · **43/44 空出来给链路** —— "USB 占了 43/44"那个麻烦彻底消失。
//
// ------------------------------------------------------------
// 三、★★ 风险（这是本文件默认关闭的**唯一**原因，必须读完）
// ------------------------------------------------------------
//   GPIO0 **同时是 BOOT strapping 脚**。ESP32-S3 的复位序列会看 `GPIO_STRAP_REG`
//   决定"从 flash 启动"还是"进 ROM 下载模式"；而这个寄存器的值正是 strapping
//   脚在复位那一刻的电平。我们一旦把 GPIO0 驱动成低，**下一次复位时它可能读到低**
//   ⇒ 芯片进下载模式：`rst:0x… ,boot:0x… (DOWNLOAD(USB/UART0))`、**app 不跑、
//   屏黑**（这一现象本项目实测过，见 ACCEPTANCE.md 的 `USB_UART_CHIP_RESET` 那条）。
//
//   两种复位的差别（**已查证**，出处：ESP-IDF v5.5「Application Startup Flow」
//   <https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/startup.html>）：
//     · ROM 启动代码按**复位原因**分支 ——
//       "For power-on reset, software SoC reset, and watchdog SoC reset: check the
//        `GPIO_STRAP_REG` register if a custom boot mode (such as UART Download Mode)
//        is requested."
//       而 "For **software CPU reset and watchdog CPU reset**: configure SPI flash …
//       and attempt to load the code from flash."（**不查 strap**）
//     · 数据手册那条更直接：strapping 的锁存器在 **power-on / RTC watchdog /
//       brownout** 这几类复位时**重新采样**。
//   ⇒ **不是所有复位都会重新采样**，但"会重新采样的那几类"（掉电重启、
//     RTC 看门狗、欠压）**恰好都是车上会真的发生的**。
//   ⇒ 结论：**复位期间 GPIO0 保持低 = 可能进下载模式**，而这条路**我们关不掉**
//     （开关的 `SEL` 是硬件端子，固件不驱动它就回到上拉默认值 = CH343P，
//      于是"想拿日志"这件事又回到要按 BOOT）。
//
//   好消息（这条同样必须写清）：**不会变砖**。进下载模式之后
//     · esptool 照样能烧（下载模式就是它要的状态）；`COM9`/`COM8` 那两个口还在；
//     · 退回一步**只要拔掉重插**（power-on 时 GPIO0 被 `R44`/`R45` 拉回默认，
//       而 app 还没跑、没有谁在驱动它 ⇒ 不按 BOOT 就是正常启动）。
//   ⇒ 所以这是"**可能要拔一次线**"的代价，不是"变砖"。
//
// ------------------------------------------------------------
// 四、设计（做了就是这样做；极性以车主实测为准）
// ------------------------------------------------------------
//   ① **方向**：`GPIO0` 配成**开漏输出**（`OUTPUT_OPEN_DRAIN`）——
//      低 = 选原生 USB（车主实测）；不驱动 = 由两个 10 K 拉回默认。
//      ★ 用开漏而不是推挽：这颗脚上与它对拉的是板上的上拉电阻与 BOOT 按键，
//        开漏只会"拉低/放开"，不会与按键对打。
//      ★ 极性如果**反了**（实测发现拉低反而变成 CH343P）：把
//        `kSelectNativeLevel` 改一个值即可，**不用改结构**。
//   ② **时机**：在 `setup()` 里、**日志起来之后**（那才能自证）、
//      且**在 43/44 被 UART0 抓走之前**（驱动单个 GPIO 与 UART 无关，
//      但顺序写在同一条注释里，免得将来有人把它挪到 `dash_display_init()` 之后）。
//   ③ **自证**：开机日志打一行 —— 它是"固件以为自己是什么人格"在串口上
//      唯一的证据；而"设备管理器里是什么"由车主看。两句话对上才算证据。
//   ④ **评估过的"更安全的写法"（三种，都没选，理由在这里）**：
//      · **只在需要时驱动**：USB 是"一直在"的，没有"需要时" ⇒ 不成立；
//      · **主动复位前先释放**：本项目**没有**主动复位（`esp_restart()`）这条路，
//        而**看门狗/欠压我们来不及释放** ⇒ 挡不住真正危险的那一类；
//      · **换一颗能锁存的开关**：动硬件，不在本单范围（且 `SEL` 已经是 GPIO0）。
//      ⇒ 所以"更安全"的那一版**只能是"不开这个开关"**，这就是默认 0 的理由。
//   ⑤ **退回路径**（做了之后万一出事，按这个顺序）：
//      · **插拔一次**：这是 power-on，strapping 重新采样，此时 app 还没跑
//        ⇒ 不按 BOOT 就是正常启动（日志照旧走原生 USB —— 插拔那一下开关是默认态，
//          所以**日志从 CH343P 那一路看不到**，要看日志就按住 BOOT 插一次）；
//      · 要看 app 日志但手上没有 BOOT：把这一版固件**重新烧一遍**（esptool 在
//        下载模式下照烧），烧完插拔一次即可；
//      · 彻底不要这个功能：把 `-DUSB_PERSONALITY_AUTO=1` 去掉（= 回到本文件默认）。
//
// ------------------------------------------------------------
// 五、判据（做了才谈得上；现在**一条都没有**，因为默认没开）
// ------------------------------------------------------------
//   ① 正常插电（**不按 BOOT**）⇒ 设备管理器里出现**原生 USB**（`USB 串行设备`）；
//   ② 开机日志里那一行"人格"与 ① 对上；
//   ③ **43/44 上的链路照常工作**（至少要有"43/44 没被 USB 占"的证据：
//      日志里 link 的 TX/RX 计数在动）；
//   ④ 复位/重插多次，**从没进过下载模式** —— 每次贴 `boot: reason=… raw=…` 那一行。
//   ★★ **判据 ④ 目前给不出来**（这正是没开的原因）：要证明"RTC 看门狗/欠压复位
//      不会把它带进下载模式"，必须真去制造那几类复位，而每一次尝试都有
//      "板子停在下载模式、要车主去拔线"的代价 —— 那不该由我在没人看的台面上做。
// ============================================================

#ifndef USB_PERSONALITY_AUTO
// ★ 默认 **0 = 不驱动 GPIO0**（= 与今天的行为逐字节相同）。
//   开启方式：给需要的那一份 env 加 `-DUSB_PERSONALITY_AUTO=1`
//   （`platformio.ini` 本单**未动**；开启属于"要 owner 拍板"的那一步，见第四节）。
#define USB_PERSONALITY_AUTO 0
#endif

#if (USB_PERSONALITY_AUTO != 0) && (USB_PERSONALITY_AUTO != 1)
#error "USB_PERSONALITY_AUTO 只能是 0(不驱动 GPIO0) 或 1(开机选原生 USB)"
#endif

namespace dashusb {

// `FSUSB42UMX` 的 `SEL` 挂在**这一根**脚上（厂商图：网络 `UART0SEL` = `IO0`）。
static const int8_t kSwitchSelPin = 0;

// 选"原生 USB"的那一侧电平。★ 极性出处是**车主实测**：按住 BOOT（= 把 GPIO0 拉低）
//   插电 ⇒ 枚举成原生 USB ⇒ `SEL` 低 = 原生 USB。
//   ★ 如果哪天实测反了，**只改这一个值**：把 0 改成 1（结构不用动）。
static const uint8_t kSelectNativeLevel = 0;

// 只认 0/1（写别的值一定是把它当成"电平"以外的东西了）。
static_assert(kSelectNativeLevel == 0 || kSelectNativeLevel == 1,
              "kSelectNativeLevel 只能是 0(低=原生 USB) 或 1(高=原生 USB)");
static_assert(kSwitchSelPin == 0,
              "SEL 就在 GPIO0 上（网络 UART0SEL）；换脚等于换板子 —— 改这一条前先看厂商图");

// 这份固件会不会在 setup() 里驱动 GPIO0（编译期事实，日志与用例都读它）。
static const bool kAutoSelectEnabled = (USB_PERSONALITY_AUTO != 0);

// 人格的纯 ASCII 名字（开机日志那一行用；本构建只使能 Montserrat，
// 而且 GBK 控制台对中文会抛 UnicodeEncodeError ⇒ 这条纪律与别处一样）。
// ★ "固件**要求**哪一边"与"设备管理器里**实际**是什么"是两件事：
//   前者由这个字符串说，后者只有插上机器才知道 —— 两者必须对上才算证据。
inline const char* requestedPersonalityName() {
#if USB_PERSONALITY_AUTO
  return "native-usb(GPIO19/20, log+power)";
#else
  return "ch343p(43/44 to the on-board bridge)";
#endif
}

}  // namespace dashusb

// ---- 设备端实现（宿主机与 pcpreview 不编：那里没有 GPIO） ----
// ★ 放在头文件里、用 `#if defined(ARDUINO)` 门住，是为了让"驱动它"这件事
//   与"描述它"的那几个常量待在**同一个文件**里（两处 = 迟早分叉）。
// ★ 返回 true 表示这次真的驱动了 GPIO0（= 开关被扳到原生 USB 那一边）。
#if defined(ARDUINO)
#include <Arduino.h>
inline bool usb_personality_select() {
#if USB_PERSONALITY_AUTO
  // ★★ 开漏输出：只会"拉低 / 放开"，不会与板上的上拉电阻和 BOOT 按键对打。
  pinMode(dashusb::kSwitchSelPin, OUTPUT_OPEN_DRAIN);
  digitalWrite(dashusb::kSwitchSelPin,
               dashusb::kSelectNativeLevel ? HIGH : LOW);
  return true;
#else
  // 默认那一路：**一个寄存器都不碰**（GPIO0 保持复位后的输入态，
  // 由板上两个 10 K 决定开关倒向哪边 —— 与今天的行为逐字节相同）。
  return false;
#endif
}
#endif  // defined(ARDUINO)

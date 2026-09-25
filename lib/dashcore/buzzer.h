#pragma once
#include <stdint.h>
#include "alerts.h"

// ============================================================
// 蜂鸣器抽象 —— "谁发声"与"什么时候该响"分开
//
// ★★ 这一层存在的唯一理由，是 ARCHITECTURE.md §8 的 **L14**
//    「将来那个声音告警由哪块板发声」。本轮按 **"从板本地发声"** 实现：
//      · 红区是**从板**那一屏的档位（左屏 = 转速表）⇒ 主板上算出来的告警，
//        要从板自己响，就得先把档位/事件经 `EVENT 0x01` 上行（§3 的号段本来
//        就为它留着），那条路 v1 还没定（L14 本身就是"待确认"）；
//      · 而 2.8C 板上**已经有一个不用额外引脚的蜂鸣器**：TCA9554 的 `EXIO8`
//        （同一颗扩展芯片已经在驱动那块屏的 20 根线，见 ARCHITECTURE
//        「接线定案」表最后一行）。
//    ⚠ **这是"建议"，不是"已裁决"** —— owner 还没点头。文档里也照这个措辞写
//      （`ARCHITECTURE.md` §8 的 L14 行 + `tools/theme-editor/README.md` 的素材节）。
//      因为一旦 L14 最终判给"主板发声"，要换的只是 main.cpp 里挂哪个实现，
//      本文件与 alerts.* 一个字都不用动 —— 这正是这一层的用处。
//
// ★ 分层口径（别把两件事混了）：
//     Alerts  决定"该不该响、响什么模式"（纯逻辑，native 可测）
//     Buzzer  决定"这个模式在**这块板**上怎么落"（频率/占空比/提示音）
//   所以 Buzzer 只有一个方法、不持有任何状态机：它不判断、不记忆。
// ============================================================

class Buzzer {
public:
  virtual ~Buzzer() = default;

  // 一次性初始化（引脚方向 / 寄存器配置）。允许重复调用。
  virtual void begin() = 0;

  // 按模式响一拍。`beep_ms` 是**标称时长**（来自 AlertsConfig.beep_ms）——
  // ★ 实现**不必**精确按它阻塞：真机上大多是"开 → 由调用方稍后关"，
  //   由 `off()` 收尾（见下面 off() 的说明）。
  virtual void beep(BeepPattern pattern, uint32_t beep_ms) = 0;

  // ★ 2026-09-24（接真机那一档时新增）：**非阻塞推进**，主循环每轮调一次。
  //   为什么需要它：调用方拿到的"该不该响"是**一拍一拍**给的
  //   （`Alerts::beeping()` 只在一拍上为真、那一声轻提示更是边沿触发一次），
  //   而"多相序列"（`Triple`/`Urgent`/`Long`⇒3 短哔）要在**好几拍之间**才走得完
  //   ⇒ 必须有人在这些拍之间把时序往前推，否则序列永远停在第一相
  //   （症状：本该 3 声，实际只响 1 声）。
  //   ★ 默认**空实现**：`BuzzerNull` / `BuzzerHost` 都是"一调就落完"的，
  //     它们不需要推进 —— 所以这一条对既有实现与既有一行调用点**零影响**。
  //   ★ 它**绝不许阻塞、绝不许 `delay()`**（调用点在显示主循环上）。
  virtual void tick() {}

  // ★★ 2026-09-25（接"屏卡死 + 蜂鸣器长鸣"那一单时新增）：**绝对上限兜底**。
  //   主循环每轮调一次，与 `tick()` 并列 —— 但语义与它**完全不同**：
  //     · `tick()`   = "按序列推进时序"（到点关、下一相起表）；
  //     · `safety()` = "**不管序列在什么状态**，任何一次 `beep()` 之后只要
  //       墙钟超了上限（真机那一档是 `kBuzzerSafetyMs` = 2 s），就无条件关掉"。
  //
  //   为什么必须有它（这不是防御性代码，是车主现场的原话）：
  //     "**现在会长鸣一会儿，画面也卡住了**"。而这块板上的蜂鸣器是**软开关**
  //     （写 TCA9554 的 EXIO8，**没有硬件定时**）⇒ "到点关"这个动作**只在主循环
  //     转得动的时候**才会被执行。主循环一旦停住（渲染被堵、I2C 挂住、某段长循环），
  //     `tick()` 就再也不会被调用 ⇒ 高电平**留在了总线上**，谁也关不掉。
  //     ⇒ 所以这一条是**独立于序列状态机**的第二道判据：它只信"起表时刻 + 墙钟"。
  //   ★ 它**不动**既有那条"单次哔 ≤ kMaxPulseMs(300ms)"的硬约束（序列自己照旧按相走、
  //     每个相照旧被夹）—— 这一条只在"那段逻辑没机会跑"时才生效，是天花板不是替代。
  //   ★ 默认**空实现**（`BuzzerNull` / `BuzzerHost`）：它们"一调就落完"、不留高电平
  //     ⇒ 对既有实现与既有一行调用点**零影响**；真机那一档（`BuzzerExio`）才有实体。
  //   ★ 同样**绝不许阻塞、绝不许 `delay()`**（调用点在显示主循环上）。
  virtual void safety() {}

  // 收尾：把输出拉回静默。**每拍都要调**，与 beep() 成对 ——
  // 这样"实现按自己的节奏决定响多久"与"上层不阻塞"能同时成立。
  virtual void off() = 0;

  virtual const char* name() const = 0;
};

// ---- 什么都不做（默认）：没有蜂鸣器的板子 / 单测 ----
class BuzzerNull : public Buzzer {
public:
  void begin() override {}
  void beep(BeepPattern, uint32_t) override {}
  void off() override {}
  const char* name() const override { return "null"; }
};

// ---- 宿主机（pcpreview）：打印一行 + 可选系统提示音 ----
// ★ 为什么默认只打印不发声：本轮**没有任何硬件**，而"在开发机上突然响一声"
//   对跑测试/跑构建的人是干扰。要看/要听的效果由这一行日志保证：
//   `BEEP pattern=triple ms=120`。想真出声就 `-DBUZZER_HOST_SOUND=1`
//   （Windows 上走 MessageBeep，不改任何判据）。
class BuzzerHost : public Buzzer {
public:
  void begin() override;
  void beep(BeepPattern pattern, uint32_t beep_ms) override;
  void off() override {}
  const char* name() const override { return "host"; }
};

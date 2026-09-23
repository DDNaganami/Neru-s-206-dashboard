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

#pragma once
#include <stdint.h>
#include "buzzer.h"   // BeepPattern + Buzzer 接口（本文件只加"怎么落"，不动那一层）

// ============================================================
// 真机那一档的蜂鸣器 —— 2.8C 板载**有源**蜂鸣器，挂在 TCA9554 的 EXIO8
//
// 结论与出处（不是猜的，别改回去）：
//   · 有源/无源的实测在 `docs/RGB-PANEL-2.8C.md` §13.4 与 §13.6（车主听感 + 结构原因）；
//   · 控制脚 = `TCA9554`（I2C 扩展器）的 **EXIO8**，零额外引脚
//     （同一颗芯片本来就在驱动那块屏的 RST/CS）；
//   · ⇒ 只有"**开 / 不响**"两个自由度：**没有音调、没有音量、没有占空比**。
//     所以本类**故意不接**频率/音量参数 —— 提示音只能靠**节奏与次数**区分。
//
// ★★ 三条硬约束（每一条都对着一次真实事故或一条未排除的风险，违反就是回退）：
//
//   ① **单次哔 ≤ kMaxPulseMs（300ms）** —— 硬上限，`beep()` 里会**夹**，
//      序列构造里也再夹一次。理由：那次"屏幕突然全黑"事故有两条候选机制，
//      A（改共享 I2C 时钟打错字节 ⇒ 复位面板）已排除，
//      但 **B（持续高电平 2 秒把 3.3V 轨拉低 ⇒ 面板掉状态）仍未排除**
//      ⇒ 在 B 排除前**不许出现长时间连续高电平**（`ARCHITECTURE.md` §4 第 2/3 条）。
//
//   ② **`BeepPattern::Long` 在真机上降级成 3 短哔**（不是长鸣）——
//      这是 ① 在"模式"这一层的落法：`Long` 那个模式名仍然在（`alerts` 与预览
//      照旧用它），但**落到这块板上**就是 3 声短哔。见 `pulsesFor()`。
//      注：**`Urgent` 是 4 声**（不是 3 声）—— 与 `Triple`/`Long` 只差一声，
//      这是有源蜂鸣器"只有次数"这一条自由度下的已知代价
//      （见 `ARCHITECTURE.md` §4 的词表；本节原先把 `Urgent` 写成 3 声，是文档错，
//       2026-09-24 按 `pulsesFor()` 更正）。
//
//   ③ **时序由注入进来的时钟给，不走 `millis()`、不用 `delay()`** ——
//      ① 便于 native 用例用**假时钟**逐格推进（真 `millis()` 在宿主机上不可控）；
//      ② 调用方（`main.cpp` 的主循环）本来就是"每轮 poll 一次"，
//         **一次都不阻塞**（RGB 那条时间线对额外占用很敏感，见 §12）。
//
// ★★ 输出那一侧是**注入的回调**，本文件**不 include 任何 I2C/Arduino 头**：
//   · 真机上那是 `src/dash_display_rgb.cpp` 的 `dash_buzzer_set()`（唯一的写者，
//     拿着影子寄存器做读-改-写）；
//   · native 用例里是一个假端口（记录每一次开/关），于是
//     "**只有目标位变、RST/CS 一个字节都没动**"这件事能在宿主机上被钉住。
//   ★ 为什么不让这个类自己去写 TCA9554：那颗芯片的输出寄存器里**同时挂着
//     `LCD_RST`(EXIO1) 与 `LCD_CS`(EXIO3)** ⇒ 写错一位就是一次**面板复位**，
//     而面板复位**不会自己回来**。影子寄存器只能有**一份**（两处各持一份会互相
//     覆盖丢位）—— 那一份在显示驱动里，这里只调它的入口。
// ============================================================

// 单次哔的硬上限（ms）。★ 与 `ARCHITECTURE.md` §4 第 2 条同一个数。
constexpr uint32_t kMaxPulseMs = 300u;
// 序列里"两哔之间"的静音：**节奏**是这块板上唯一还剩的表达手段，
// 所以这一拍必须是**听得出来**的（太小就成了"一长声"）。只做短哔的告警用不到它。
constexpr uint32_t kGapMs = 80u;

// 一个模式的"落法"：最多几次开、每次多长。
// ★ `on_ms == 0` 的槽位不参与相数（它不是一次发声）；
//   `on_ms > 0` 的槽位个数 = **这一拍要响几声**，用例直接钉这个数。
struct BeepPulses {
  uint32_t on_ms[8];
  uint8_t  n;
};

// 把"模式 + 标称时长"落成这块板上的相数。
// ★ 返回值里每个非零 `on_ms` 都 ≤ kMaxPulseMs（本函数负责夹）。
//   `Silent` ⇒ n=0（一声都不响）；`Short`/`Triple` ⇒ 1/3 声；
//   `Urgent` ⇒ **4** 声；`Long` ⇒ **3 声**（降级，见文件头 ②）。
BeepPulses pulsesFor(BeepPattern pattern, uint32_t beep_ms);

// 一次"开关"请求。★ 只有这一个动作 —— 有源蜂鸣器没有别的可做。
typedef void (*BuzzerExioSetFn)(bool on, void* ctx);

class BuzzerExio : public Buzzer {
public:
  BuzzerExio(BuzzerExioSetFn set_fn, void* ctx, uint32_t (*now_fn)());

  // 一次性初始化：允许重复调用；把它拉到**已知的静音**（不记忆任何硬件状态）。
  void begin() override;

  // 起一拍。★ 非阻塞：只记下"这一拍长什么样 + 从什么时候起算"，
  // 真正的高低电平由调用方每轮调 `tick()` 推进（见文件头 ③）。
  // ★ 起表之后的**第一次 `tick()` 就起第一声**（与两轮之间隔了多久无关）——
  //   RGB 那条路上"起表 → 第一次 tick"正好会跨过一次几十毫秒的 flush，
  //   把第一声交给计时逻辑去走会让单相序列**永远不响**（见 .cpp 里那段说明）。
  // ★ 同一拍里被**重复调用**（主循环每轮都调，见 `main.cpp`）时**不会重新起表**
  //   —— 否则每次调用都把相位归零，序列永远走不完（那种 bug 看起来像"只响了一声"）。
  //   "重新起表"只有两种情况：① 上一个序列已经走完；② 模式或时长变了。
  void beep(BeepPattern pattern, uint32_t beep_ms) override;

  // 收尾/取消：把输出拉回静默，并**丢掉**当前序列。
  // ★ 它必须能**立刻**掐断（静音那一跳用的是这一条：`main.cpp` 在"不该响"的
  //   那些轮里调它）—— 所以语义就是"取消"，不是"等这一拍放完"。
  void off() override;

  // 每轮推进时序（`Buzzer::tick()` 的真正实现；主循环每轮调一次）。
  // 到点就关；多相序列的下一相在**上一相结束之后**起表
  // （相位从"真的换过去"那一刻算，不是按格点排 —— 晚一拍只是把整段拉长，
  //  不会把两次发声粘成一次，也不会让"响"的那一相被提前掐掉）。
  void tick() override;

  const char* name() const override { return "exio8"; }

  // ---- 用例/日志要看的几个读数（只读，不改行为）----
  bool     pulseActive() const;      // 这一刻是不是"正在响"
  uint8_t  pulseIndex() const { return pulse_idx_; }   // 已起了几相（0 = 还没起）
  uint8_t  pulseCount() const { return pulses_.n; }    // 这一拍一共几相
  bool     sequenceActive() const { return active_; }
  uint32_t lastPulseMs() const { return last_pulse_ms_; }   // 最近一次**夹过**的相时长

private:
  void startSequence(const BeepPulses& ps);
  void clearSequence();   // 丢掉序列 + 幂等缓存（★ 缓存必须一起清，见 .cpp）
  void allOff();

  BuzzerExioSetFn set_ = nullptr;
  void* ctx_ = nullptr;
  uint32_t (*now_)() = nullptr;

  BeepPulses pulses_{};       // 当前这一拍的落法
  uint8_t  pulse_idx_ = 0;    // 已起了几相（= 下一相的下标）
  bool     active_ = false;   // 序列还在推进
  bool     out_on_ = false;   // ★ 我们**要求过**的电平（只在变化时才动总线）
  uint32_t pulse_start_ms_ = 0;   // 当前相**真的开始**的时刻（相内 0 也是起点）
  uint32_t last_pulse_ms_ = 0;    // 最近一次夹过的相时长（日志/用例）
  BeepPattern cur_pattern_ = BeepPattern::Silent;
  uint32_t cur_req_ms_ = 0;       // 调用方给的标称时长（判"要不要重新起表"）
};

#include "panel_guard.h"

// ============================================================
// 实现说明（与 panel_guard.h 的文件头配套读；这里只写"怎么落"）
//
// ★ 这个文件**一个 I2C/Arduino/LEDC 符号都不引用**（输出与时钟全是注入的）
//   ⇒ native 用例能逐条钉住判据，设备侧唯一的写者仍是 dash_display_rgb.cpp。
//
// ★★ 两条"看起来可以省、其实不能省"的写法：
//   ① 守护**不自己改影子寄存器**。影子是"我们要求的真值"，由显示驱动写；
//      守护发现不一致时的动作是"**按影子重写**"，不是"把读回来的值记下来当新的影子"
//      —— 后者等于把一次被改坏的位**接受**成新常态（那正是我们要修的东西）。
//   ② 背光那一路**无论如何都要读一次**（`read_duty_ != nullptr` 时），
//      即使扩展器那次读失败了：两路是**独立**的故障面（供电跌落会先干掉背光这一路），
//      一路坏了不该把另一路的体检也跳过。
// ============================================================

namespace {

// 把 `now_ms` 之后 `ms` 毫秒拼成一个截止时刻（无符号回绕安全：用差值比较）。
uint32_t deadline(uint32_t now_ms, uint32_t ms) { return now_ms + ms; }

// 这一拍到了吗？★ 用 `(int32_t)(now - due) >= 0` 而不是 `now >= due`：
//   `millis()` 49.7 天回绕一次，差值形式在回绕那一刻也是对的。
bool due(uint32_t now_ms, uint32_t due_ms) { return (int32_t)(now_ms - due_ms) >= 0; }

}  // namespace

PanelGuard::PanelGuard(PanelGuardReadExioFn  read_exio,
                       PanelGuardWriteExioFn write_exio,
                       PanelGuardReadDutyFn  read_duty,
                       PanelGuardWriteDutyFn write_duty,
                       PanelGuardReinitFn    reinit,
                       PanelGuardLogFn       log,
                       void*                 ctx)
    : read_exio_(read_exio),
      write_exio_(write_exio),
      read_duty_(read_duty),
      write_duty_(write_duty),
      reinit_(reinit),
      log_(log),
      ctx_(ctx) {}

void PanelGuard::setShadow(uint8_t shadow) { shadow_ = shadow; }

void PanelGuard::setBacklightDuty(uint32_t duty) { duty_ = duty; }

void PanelGuard::begin(uint32_t now_ms) {
  started_ = true;
  // ★ 重新起算周期（不是"立刻检查一次"）：上电头 2 秒正好在跑面板初始化，
  //   那时的扩展器/背光状态本来就在变，插一次检查只会得到假警报。
  next_ms_ = deadline(now_ms, kPanelGuardPeriodMs);
  streak_ = 0;
  req_ = false;
}

void PanelGuard::noteReinit(uint32_t now_ms) {
  streak_ = 0;
  req_ = false;                       // 已经做过了，请求作废
  last_auto_ms_ = now_ms;             // 起算冷却期
  next_ms_ = deadline(now_ms, kPanelGuardPeriodMs);
}

bool PanelGuard::takeReinitRequest() {
  const bool r = req_;
  req_ = false;
  return r;
}

void PanelGuard::tick(uint32_t now_ms) {
  if (!started_) return;                       // 没 begin 过 ⇒ 什么都不做
  if (!due(now_ms, next_ms_)) return;          // 没到点 ⇒ 零成本（几次整数比较）
  next_ms_ = deadline(now_ms, kPanelGuardPeriodMs);
  doCheck(now_ms);
}

void PanelGuard::doCheck(uint32_t now_ms) {
  ++checks_;

  // ---- ① 扩展器输出寄存器：回读 vs 影子 ----
  bool exio_bad = false;
  if (read_exio_ != nullptr) {
    uint8_t rb = 0;
    const bool ok = read_exio_(&rb, ctx_);
    if (panel_guard_exio_matches(ok, rb, shadow_)) {
      if (ok) {
        ++rd_ok_;
      } else {
        // ★ 读失败**不算**"不一致"（见 panel_guard_exio_matches 的说明）：
        //   总线无应答与"寄存器真被改了"是两件事，前者重写没有意义。
        ++rd_err_;
        if (log_ != nullptr) log_(kPanelGuardLogRdErr, 0u, 0u, ctx_);
      }
    } else {
      exio_bad = true;
      ++rd_ok_;                   // 读本身是成功的（拿到了字节）
      ++anomalies_;
      // ★★ 修复动作 = **按影子重写**（不是读-改-写、更不是接受读回来的值）。
      //    这一条正是"面板复位位（LCD_RST/LCD_CS）被意外改写"那条黑屏路径的修复。
      if (write_exio_ != nullptr) write_exio_(shadow_, ctx_);
      ++fix_exio_;
      if (log_ != nullptr) log_(kPanelGuardLogExio, (uint32_t)rb, (uint32_t)shadow_, ctx_);
    }
  }

  // ---- ② 背光：LEDC 当前占空 vs 设定值 ----
  // ★ 与①**独立**：即使①读失败，这一路照做（见文件头 ②）。
  if (read_duty_ != nullptr) {
    const uint32_t cur = read_duty_(ctx_);
    if (!panel_guard_backlight_matches(cur, duty_)) {
      exio_bad = true;            // "这一拍有异常"（连续计数的口径见下）
      ++anomalies_;
      if (write_duty_ != nullptr) write_duty_(duty_, ctx_);
      ++fix_bl_;
      if (log_ != nullptr) log_(kPanelGuardLogBl, cur, duty_, ctx_);
    }
  }

  // ---- ③ 连续异常 ⇒ 请求自动重初始化一次（**不是**周期性重初始化）----
  if (exio_bad) {
    if (streak_ < 0xFFu) ++streak_;
  } else {
    streak_ = 0;
  }
  if (streak_ >= kPanelGuardAnomalyStreak) {
    // 冷却期：刚自动重初始化过就不再赌（否则会变成"周期性重跑 41 步"⇒ 屏定期闪）。
    const bool cooled =
        (reinit_count_ == 0u) ||
        due(now_ms, deadline(last_auto_ms_, kPanelGuardAutoReinitCooldownMs));
    if (reinit_ != nullptr && cooled) {
      req_ = true;
      ++reinit_count_;
      last_auto_ms_ = now_ms;
      const uint8_t streak = streak_;
      streak_ = 0;                // 已经赌过一次，重新起算
      if (log_ != nullptr) log_(kPanelGuardLogAuto, (uint32_t)streak, 0u, ctx_);
    }
  }
}

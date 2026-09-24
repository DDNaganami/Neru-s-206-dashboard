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
  clean_ = 0;
  in_fast_ = false;
  req_ = false;
}

void PanelGuard::noteReinit(uint32_t now_ms) {
  streak_ = 0;
  // ★ 重初始化刚刚把面板/扩展器重新交代过一遍 ⇒ 退回稳态周期等它（2000ms），
  //   并且**不**认为"已经连续干净"（那要等真的读到几次一致才算）。
  clean_ = 0;
  in_fast_ = false;
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
  // ★★ 自适应周期（2026-09-24 深夜，"把最坏自愈窗从 2 秒压到 ~200ms"）：
  //   判据只有下面这一行 —— 收口在**一处**，别在别处再写一遍：
  //     "还没稳"（`in_fast_`，由 `doCheck()` 按 `stable_` 更新）⇒ 下一拍 200ms 之后；
  //     "稳了" ⇒ 下一拍回到 2000ms 稳态。
  //   ★ 为什么不按"这一拍 bad 不 bad"取：发现异常 ⇒ 按影子重写 ⇒ **同一拍里**
  //     读回来的影子已经被写回去了 ⇒ **修复那一拍往往不是 bad**，按 bad 取就会在
  //     "刚被改过一次"的下一秒退回 2000ms —— 而那正是最该多盯两眼的时候。
  //   ★ 为什么"稳了"那一拍仍按 200ms 排下一拍：见 `doCheck()` 里 ③ 那段账
  //     （要让"连续 3 次复检"真的是 3 个 200ms 窗口）。
  const bool bad = doCheck(now_ms);
  (void)bad;   // 间隔只看 `in_fast_`（`doCheck()` 已按上面那三条判据更新好它）
  next_ms_ = deadline(now_ms, in_fast_ ? kPanelGuardFastMs : kPanelGuardPeriodMs);
}

bool PanelGuard::doCheck(uint32_t now_ms) {
  ++checks_;
  last_check_ms_ = now_ms;

  // ---- ① 扩展器输出寄存器：回读 vs 影子 ----
  bool bad = false;      // "这一拍读到了坏值"（**不含**读失败 —— 见下）
  bool murky = false;    // "这一拍状态不明"（读失败）。★ 与 bad 分开：它值得多盯两眼
                         //   （进快速复检），但**不许**算进"连续异常 ⇒ 自动重初始化"
                         //   —— 总线一时不应答就去反复重跑 41 步初始化是错的。
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
        murky = true;
        if (log_ != nullptr) log_(kPanelGuardLogRdErr, 0u, 0u, ctx_);
      }
    } else {
      bad = true;
      ++rd_ok_;                   // 读本身是成功的（拿到了字节）
      ++anomalies_;
      // ★★ 修复动作 = **按影子重写**（不是读-改-写、更不是接受读回来的值）。
      //    这一条正是"面板复位位（LCD_RST/LED_CS）被意外改写"那条黑屏路径的修复。
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
      bad = true;                 // 背光被改也是"坏值"（要修、也要计数）
      ++anomalies_;
      if (write_duty_ != nullptr) write_duty_(duty_, ctx_);
      ++fix_bl_;
      if (log_ != nullptr) log_(kPanelGuardLogBl, cur, duty_, ctx_);
    }
  }

  // ---- ③ 连续异常 ⇒ 请求自动重初始化一次（**不是**周期性重初始化）----
  //   ★ 只有 `bad`（读到坏值）进这个计数；`murky`（读失败）**不进** ——
  //     它只让下一拍提前到 200ms（"状态不明，多看两眼"）。
  if (bad) {
    if (streak_ < 0xFFu) ++streak_;
    clean_ = 0;
  } else {
    streak_ = 0;
    if (clean_ < 0xFFu) ++clean_;
  }
  // ★★ 快速复检的**进出判据只有这一处**（`tick()` 照它取下一拍的间隔）。
  //   这一拍读到坏值（`bad`）**或**状态不明（`murky`，读失败）⇒ 立刻进快速复检；
  //   **修好之后连续 `kPanelGuardCleanToRelax` 次复检都干净** ⇒ 下一拍起回 2000ms。
  //   ★★ 三个坑都在这儿（前两个各踩过一次，都记着）：
  //     ① `bad ||` 一个字符都不能少 —— 只看 `clean_` 的话，发现异常之后的那**一次
  //        干净复检**会把计数加到自己身上（先 ++ 再比），于是"第 1 次复检"就被判成
  //        "稳了"；实测那次的间隔是 `2000 → 2200 → 4400`，中间两次复检全丢了。
  //     ② 比较必须是 `>`（不是 `>=`）：`clean_` 是**先按这一拍更新、再比**，
  //        而"发现异常并按影子修好"的那一拍**自己也**读到干净 ⇒ 是第 1 次干净。
  //        ⇒ `clean_ == kPanelGuardCleanToRelax` 恰好就是**最后一次**复检那一拍，
  //          此时还不能退回稳态（不然"3 次复检"只剩下 2 个 200ms 窗口）。
  //        账（正是判据要的那条）：t=2000 发现并修好 → **2200 / 2400 两次复检**（都干净，
  //        到这一拍 clean_=3 ⇒ 稳）→ **2600 起**回 2000ms 周期（`next=4600`）。
  //        ⇒ 从注入到稳态，中间是 **3 个 200ms 的窗口**，最坏发现窗就是一个窗口。
  //     ③ `in_fast_` 与间隔方向**不能写反**：true = **还在**快速复检（间隔取 200ms）。
  in_fast_ = bad || murky || (clean_ <= kPanelGuardCleanToRelax);

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
  return bad;
}

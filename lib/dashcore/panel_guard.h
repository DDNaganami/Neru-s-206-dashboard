#pragma once
#include <stdint.h>

// ============================================================
// 面板健康守护（PanelGuard）—— 2026-09-24 **产品要求**驱动的第七层
//
// ★★ 为什么要有这个东西（业主原话，逐字记在案）：
//     "屏幕回来了，**上实车的时候可不能这样，这毕竟是仪表盘，要常亮的**。"
//   ⇒ 仪表盘**黑屏 = 功能性失效**（不是"外观问题"）。而 2026-09-24 当晚真的发生过：
//     车主吃完饭回来，**屏黑了、固件一直活着**（复位前抓到的原文：
//     `rgb: vsync=68942(+54/s) … timeout=0 … fullrb=0/s` + 每秒一行 `206 dash ok`），
//     复位（RTS 脉冲）后立刻恢复。归档在 `docs/RGB-PANEL-2.8C.md` §13.8（那次是自检
//     引起的）与 `ACCEPTANCE.md`（当晚这一次）。
//
// ★★ 三层防线里的**第二层**（见 `ARCHITECTURE.md`「仪表盘必须常亮」那一节）：
//     ① **根因消除**：不许在运行期改共享 I2C 时钟、不许长鸣（已做）；
//     ② **可检测故障 ⇒ 自愈**：**本文件**；
//     ③ **不可检测故障 ⇒ 现场恢复路径**：串口 `r` 命令（重跑面板初始化）+ 复位原因
//        可观测（`esp_reset_reason()` 开机一行）。
//
// ★★ 本层能查出什么、**查不出什么**（这条边界必须写在最显眼的地方）：
//   · **查得出**：那颗 TCA9554 的**输出寄存器**与我们的影子寄存器不一致
//     —— 也就是 `LCD_RST`(EXIO1) / `LCD_CS`(EXIO3) 这两位被意外改写的**最可能路径**。
//     ST7701 没有状态回读脚，所以"玻璃到底黑了没有"**软件无法直接感知**：
//     本层查的是**因**（面板被复位的那个位），不是**果**（玻璃）。
//   · **查不出**：面板因为供电跌落（3.3V 轨被拉低 ⇒ 内部寄存器/电荷泵掉状态）
//     而丢初始化，而扩展器寄存器**一个位都没变** —— 那种黑屏本层**看不见**，
//     只能靠第 ③ 层（现场 `r` 命令）+ 复位原因（BROWNout）来定位。
//   · 背光这一路同理：查的是"LEDC 占空比还对不对"，不是"灯珠还亮不亮"。
//
// ★★ 输出与时钟都是**注入的回调**（与 `buzzer_exio.h` 逐字同一条纪律）：
//   本文件**不 include 任何 I2C / Arduino / LEDC 头**，于是：
//     · native 用例能用**假端口 + 假时钟**把"一致 ⇒ 不写 / 不一致 ⇒ 只按影子重写一次"
//       这类判据逐条钉死；
//     · 设备侧唯一的写者仍然是 `src/dash_display_rgb.cpp`（那份影子寄存器只有一份）。
//
// ★★ 频率与代价（**不许放进热路径**）：
//   · 检查周期 **2000ms**（低频；车主看不到、也不占显示时间线）；
//   · 每次检查是**一次 I2C 读**，实测那种事务在本板 100kHz 下 ~400µs
//     （见 §13.8.4 的"事务 392~429µs/次"）⇒ 摊到 2 秒里可以忽略；
//   · `tick(now_ms)` **只被调用、不做等待**，时钟由调用方给（与 buzzer_exio 同一条）。
// ============================================================

// ---- 默认周期（ms）。★ 判据"守护在约 2 秒内发现"就是这一个数 ----
static const uint32_t kPanelGuardPeriodMs = 2000u;

// ---- 连续多少次检查都异常 ⇒ 请求自动重初始化一次 ----
// ★ 为什么是"连续"，以及为什么**不是周期性重初始化**：
//   周期性重跑 41 步初始化会让屏**定期闪**（那本身就是仪表盘上的故障现象）。
//   只有"连续 N 次都修不好"这种**持续异常**才值得赌一次重初始化；
//   而且重初始化之后有冷却期（`kPanelGuardAutoReinitCooldownMs`），不会来回折腾。
static const uint8_t  kPanelGuardAnomalyStreak    = 3u;
static const uint32_t kPanelGuardAutoReinitCooldownMs = 60000u;

// EXIO 输出寄存器的**纯判据**（注入回读的返回值与影子）。
// ★ `read_ok == false` ⇒ 返回 false：读失败**不算**"不一致"
//   —— 总线一时无应答与"寄存器真的被改了"是两件事，前者重写一遍没有意义
//   （而且会在总线坏了的时候每 2 秒写一次）。读失败单独计数（`rd_err`）。
inline bool panel_guard_exio_matches(bool read_ok, uint8_t readback, uint8_t shadow) {
  if (!read_ok) return true;
  return readback == shadow;
}

// 背光的**纯判据**：设定值读回来不一样就是要重设。
// ★ 没有"读失败"这一档：LEDC 是片上外设，读它就是读自己的寄存器。
inline bool panel_guard_backlight_matches(uint32_t read_duty, uint32_t want_duty) {
  return read_duty == want_duty;
}

// ---- 注入点（全部由显示驱动那一侧实现，见 src/dash_display_rgb.cpp）----
// 读 TCA9554 的输出寄存器。返回 false = 这次读**没成功**（无应答/字节数不对）。
typedef bool (*PanelGuardReadExioFn)(uint8_t* out, void* ctx);
// **按影子重写**输出寄存器。★ 语义是"把影子写下去"，**不是**读-改-写：
//   影子才是我们要求的真值，这一条正是"面板复位位被改写"的修复动作。
typedef void (*PanelGuardWriteExioFn)(uint8_t shadow, void* ctx);
// 读 LEDC 当前占空比（`ledcRead()`）。
typedef uint32_t (*PanelGuardReadDutyFn)(void* ctx);
// 重设 LEDC 占空比（`ledcWrite()`）。
typedef void (*PanelGuardWriteDutyFn)(uint32_t duty, void* ctx);
// 面板重初始化（重跑 ST7701 的 41 步 + 重发当前 framebuffer）。
// ★ 由显示驱动实现；**返回 void** —— 成没成由驱动自己那几行日志自证。
typedef void (*PanelGuardReinitFn)(void* ctx);
// 打一行日志。★ **不自己格式化**（这个文件里没有 snprintf）：数值由显示驱动那一侧
//   `dash_logf` 的 `%u` / `%02X` 打出来 —— 本文件只管"哪一类事件、两个数是什么"。
//   `which` 的取值见下面的 `kPanelGuardLog*`。
typedef void (*PanelGuardLogFn)(uint8_t which, uint32_t a, uint32_t b, void* ctx);

// 日志事件的种类（`PanelGuardLogFn` 的第一个参数）。
// ★ 驱动侧把它们落成这四行**固定字样**（文档/判据里逐字引用，别改）：
//     kPanelGuardLogRdErr : `panelguard: exio rd err (no ack) -> retry next`
//     kPanelGuardLogExio  : `panelguard: exio rd=0xXX shadow=0xYY -> rewritten`
//     kPanelGuardLogBl    : `panelguard: backlight rd=NNN want=NNN -> rewritten`
//     kPanelGuardLogAuto  : `panelguard: anomaly xN -> auto reinit`
static const uint8_t kPanelGuardLogRdErr = 0u;
static const uint8_t kPanelGuardLogExio  = 1u;
static const uint8_t kPanelGuardLogBl    = 2u;
static const uint8_t kPanelGuardLogAuto  = 3u;

class PanelGuard {
 public:
  // 注入。`reinit`/`log` 允许为 nullptr（那就没有自动重初始化、也没有日志）。
  PanelGuard(PanelGuardReadExioFn  read_exio,
             PanelGuardWriteExioFn write_exio,
             PanelGuardReadDutyFn  read_duty,
             PanelGuardWriteDutyFn write_duty,
             PanelGuardReinitFn    reinit,
             PanelGuardLogFn       log,
             void*                 ctx);

  // 记下"我们要求的"两个值。★ 允许运行期再调（背光档位若哪天可变）：
  //   调了之后下一次检查就按新值对账。
  void setShadow(uint8_t shadow);
  void setBacklightDuty(uint32_t duty);

  // 上线：允许重复调用。★ 会**重新起算**周期（第一次检查在 `now_ms + 周期` 之后），
  // 于是"开机后头 2 秒"不会被一次检查插进来（那 2 秒正好在跑初始化）。
  void begin(uint32_t now_ms);

  // 每轮推进一次（主循环每个 tick 调一次）。
  // ★ 非阻塞、**不做 I2C 以外的任何事**；没到点就是几次整数比较。
  // ★ 到点那一次做三件事：① 读回输出寄存器并按影子对账（不一致 ⇒ 按影子重写）；
  //   ② 读回背光占空比（不一致 ⇒ 重设）；③ 判"连续异常够不够多 ⇒ 请求重初始化"。
  void tick(uint32_t now_ms);

  // 显示驱动"刚刚重初始化完"时告诉守护一声。
  // ★ 它会清掉连续异常计数并起算冷却期 —— 否则下一次失败检查会立刻又请求一次。
  // ★ 它**不**增加 `reinit` 计数：那个数是"守护自己触发的自动恢复"次数，
  //   命令触发的恢复由调用方自己打日志（`panel: reinit by cmd r`）。
  void noteReinit(uint32_t now_ms);

  // ---- 只读读数（诊断页 / 日志 / 用例用）----
  uint32_t rdOk()      const { return rd_ok_; }       // 回读成功的次数
  uint32_t rdErr()     const { return rd_err_; }      // 回读失败的次数（无应答等）
  uint32_t fixExio()   const { return fix_exio_; }    // 按影子重写了扩展器的次数
  uint32_t fixBl()     const { return fix_bl_; }      // 背光被重设的次数
  // ★ 名字带 `Count`：`reinit()` 这个名字已经被上面那个**回调指针**占了，
  //   两者同名会被编译器当成"重复成员"（这个坑当场踩过一次）。
  uint32_t reinitCount() const { return reinit_count_; }  // 自动重初始化的次数
  uint32_t anomalies() const { return anomalies_; }   // 不一致的**检查次数**（含已修的）
  uint8_t  streak()    const { return streak_; }      // 当前连续异常次数
  uint8_t  shadow()    const { return shadow_; }      // 当前要求的扩展器输出
  uint32_t duty()      const { return duty_; }        // 当前要求的背光占空比
  uint32_t checks()    const { return checks_; }      // 到点后真正做过的检查次数

  // 这一拍有没有"守护请求重初始化"？★ 读一次就清（边沿语义）：
  //   主循环看到 true ⇒ 在**安全时刻**（LVGL 那一拍之外）执行重初始化。
  //   ★ 为什么要这个标志而不是在 tick 里直接重初始化：重初始化要跑 41 步初始化命令、
  //     还会重发一整屏（450KB）—— 放在"I2C 检查"这一刻做会把主循环的一拍拉得很长，
  //     而调用方（`dash_display_poll()` 的开头）正是那个"可以慢一拍"的安全时刻。
  bool     takeReinitRequest();

 private:
  void doCheck(uint32_t now_ms);

  PanelGuardReadExioFn  read_exio_  = nullptr;
  PanelGuardWriteExioFn write_exio_ = nullptr;
  PanelGuardReadDutyFn  read_duty_  = nullptr;
  PanelGuardWriteDutyFn write_duty_ = nullptr;
  PanelGuardReinitFn    reinit_     = nullptr;
  PanelGuardLogFn       log_        = nullptr;
  void*                 ctx_        = nullptr;

  uint8_t  shadow_ = 0x00;      // 我们要求的扩展器输出
  uint32_t duty_   = 0;         // 我们要求的背光占空比
  uint32_t next_ms_ = 0;        // 下一次检查的时刻
  uint32_t last_auto_ms_ = 0;   // 最近一次**自动**重初始化的时刻（冷却期用）
  bool     started_ = false;

  uint8_t  streak_ = 0;         // 连续异常次数
  bool     req_ = false;        // 待处理的重初始化请求

  uint32_t rd_ok_ = 0, rd_err_ = 0, fix_exio_ = 0, fix_bl_ = 0;
  uint32_t reinit_count_ = 0, anomalies_ = 0, checks_ = 0;
};

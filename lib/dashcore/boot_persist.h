#pragma once
#include <stdint.h>

// ============================================================
// ★★ 跨重启留档（BootPersist）—— 2026-09-25 新增，第 ③ 层防线的**取证**那一半
//
// ★ **起因（本单要解决的那个悬案，逐字记在案）**：
//   累计启动次数在夜里从 **31 涨到 32**（多了一次），而串口上只看到**当前**这次开机的
//   `boot: reason=POWERON (raw=1)` —— **上一次的复位原因没有留档** ⇒
//   "那次到底是 USB 被断电（POWERON）还是板子自己掉电（BROWNOUT）"**判不出来**。
//   这一层就是把"上一次"补上：开机那行同时打**上一次的 reason**、**上一次运行了多久**、
//   以及**守护的跨重启累计计数**。
//
// ★★ 为什么"运行了多久"要靠**周期性心跳**（这一条是本文件的中心设计）：
//   `esp_reset_reason()` 只在**开机那一次**可读（它说的是"这一次为什么起来"），
//   而"上一次跑了多久"这件事在掉电的那一刻**没有任何人来得及记**（掉电没有中断、
//   没有 atexit、BROWNOUT 也不给时间）⇒ 唯一可行的办法是**在运行期不断地把
//   "到现在为止跑了多久"写进 NVS**。
//
//   ★ 周期取 **10 分钟**（`kBootHeartbeatMs`），判据/理由：
//     · 它回答的问题有两档，而两档都能被 10 分钟的分辨率分开：
//         ① **长跑后断电**（正常）：上次跑了几小时 ⇒ `prev_up=…min` 是一个大数；
//         ② **几分钟就重启**（异常）：上次只跑了三五分钟 ⇒ 小数，
//            而"跑不到 10 分钟"的那一档必然落在最近一次心跳之前（`prev_up < 10min`）。
//     · 代价：NVS 有**磨损均衡**（ESP-IDF 的 nvs 是日志式 + 页轮转，不是原地覆写），
//       而 10 分钟一次 ≈ **144 次/天 ≈ 5.3 万次/年** —— 对 flash 的擦写寿命可接受；
//       **不要**做成每秒/每分钟 ✗（那是 3100 万次/年，白耗寿命、什么也换不来：
//       心跳只回答"上一段大概多长"，不需要秒级分辨率）。
//     · ★ 另一条代价（要说清）：`prev_up` 的**分辨率就是心跳周期** ——
//       上一次运行 27 分钟，下一次开机看到的是 `27min`（可能是 20~29 之间的某个失真值，
//       取决于最后一次心跳落在哪）。够用来分"长跑 vs 几分钟"，**不够**用来做秒级计时。
//
// ★★ 为什么计数要**跨重启累计**：
//   守护（`panel_guard.h`）的四个计数（rd/fix/bl/anom）原来只在 RAM 里 ⇒ **一重启就归零**
//   ⇒ "某次夜里守护救过几回"这件事**随重启丢失**，正好在黑屏事故最需要它的时候丢掉。
//   本层把"上次开机为止的累计"存进 NVS，开机读回来当基线，并**搭心跳的车**定期落盘
//   （所以累计值最多丢"最后一次落盘之后的那一段"，而**上一段运行结束时的累计**
//   是完整的 —— 因为基线在开机时就写下去了）。
//
// ★★ 分层纪律（与 `panel_guard.h` / `buzzer_exio.h` **逐字同一条**）：
//   · NVS 的读/写是**注入的回调** —— 本文件**不 include 任何 Arduino/NVS 头**；
//     · 于是"到 10 分钟才写心跳""上一段运行时长怎么算""计数怎么累加""跨重启怎么恢复"
//       这四条判据全部能在**宿主机**上用假 NVS + 假时钟逐条钉死；
//     · 设备侧唯一的实现是 `src/main.cpp` 里的两个 `Preferences` 回调。
//   · 时钟与心跳都由调用方给（`tick(now_ms)`），本文件**不做等待、不看 millis()**。
//
// ★ 命名空间**复用既有的 `dash`**（静音开关 `mute` 与启动次数 `bootn` 都在那里）——
//   不新开命名空间：一块板上的持久化状态只有这一处有主，分散开以后没人说得清
//   "擦掉 NVS 会丢什么"。
// ============================================================

// ---- 心跳周期（ms）。★ 判据"上一段跑了几分钟"就靠这一个数 ----
//   ★ 10 分钟 = 144 次/天 ≈ 5.3 万次/年（磨损均衡下可接受），见文件头那段账。
static const uint32_t kBootHeartbeatMs = 600000u;

// 上一段运行时长**拿不到**时用的哨兵（不是 0 —— 0 是一个合法读数）。
// ★ 与 `system_status.h` 的 `van_age_ms = UINT32_MAX` 同一条纪律：
//   "没有记录"与"记录是 0"必须能分开，否则屏上/日志里会出现一个看着像数据的假值。
static const uint32_t kBootUpUnknown = UINT32_MAX;

// ---- NVS 键（命名空间 `dash`）----
// ★ 加一个键就在这里加一行 —— 键名集中在一处，"NVS 里到底有哪些键"是可读的。
//   ★ NVS 的键名上限是 **15 个字符**（ESP-IDF 的编译期约束）。
enum class BootKey : uint8_t {
  BootCount = 0,      // "bootn"     —— 上电/复位**累计次数**（既有键，别改名：改了等于清零）
  PrevReason,         // "b_prevr"   —— 上一次开机的 `esp_reset_reason()` 名字（ASCII 短串）
  PrevReasonRaw,      // "b_prevn"   —— 它的原始枚举值（名字与 IDF 版本对不上时还能查表）
  PrevUpMs,           // "b_up_ms"   —— ★ 上一次运行**最后一次落盘**的 uptime(ms)
  GuardRd,            // "b_grd"     —— 守护：回读成功次数（累计基线）
  GuardFix,           // "b_gfix"    —— 守护：按影子重写扩展器的次数
  GuardBl,            // "b_gbl"     —— 守护：背光被重设的次数
  GuardAnom,          // "b_ganm"    —— 守护：发现不一致的次数
  GuardSnaps,         // "b_gsnap"   —— 守护累计值落盘过几次（跨重启单调 +1 ⇒ 一眼看出计数新不新）
  HeartbeatMs,        // "b_hb_ms"   —— 最近一次心跳时**本次开机已运行** ms
  HeartbeatN,         // "b_hb_n"    —— 心跳写过几次（"心跳到底有没有在跑"的判据）
  Count
};

// NVS 读：返回 **false = 这个键没有 / 读失败**（两者在判据上同义：都是"没有记录"）。
// ★ 为什么不带"错误码"：调用方对"没有记录"只有一种反应（写默认值 / 报 `-`），
//   而把 err 与 not-found 分开会让设备侧回调多一层转译，收益为零。
typedef bool (*BootNvsReadFn)(BootKey key, uint32_t* out, void* ctx);
// NVS 写：返回 false = 这次写没成功。★ **失败不算致命**：
//   这一版的信息只用于"事后取证"，丢一次心跳只损失一段时长读数，不该影响仪表盘。
typedef bool (*BootNvsWriteFn)(BootKey key, uint32_t v, void* ctx);
// 名字（`prev=` 那一格）的读写单独一条：它是一小段 ASCII，不是整数。
typedef bool (*BootNvsReadStrFn)(BootKey key, char* out, uint32_t cap, void* ctx);
typedef bool (*BootNvsWriteStrFn)(BootKey key, const char* s, void* ctx);

// ---- 开机那一刻读到的"上一次"是什么 ----
struct BootInfo {
  // 本次开机的复位原因（由调用方从 `esp_reset_reason()` 填进来）。
  uint32_t reason_raw = 0;
  const char* reason_name = "UNKNOWN";
  // ★ 上一次开机的复位原因。`prev_valid == false` ⇒ 没有记录（新板 / 第一次跑带本层的固件）。
  bool     prev_valid = false;
  uint32_t prev_reason_raw = 0;
  char     prev_reason_name[12] = "";      // 最长 "TASK_WDT"(8) + 余量
  // ★ 上一次运行了多久（ms）。`kBootUpUnknown` = 没有记录。
  //   算法：`max(最后一次心跳记下的 uptime, 上一次开机时落盘的 uptime)`
  //   —— 取两者更大的那个。
  //   ★★ 口径（说清楚，免得读歪）：它是**下界**，分辨率就是心跳周期。例：
  //       上一次开机后 12 分 42 秒被复位，而它的最后一次心跳在 10 分整
  //       ⇒ 下一次开机读到的是 `prev_up=10min`（**不是** 12min）—— 报出来的是
  //         "**至少**跑了 10 分钟"（真机上实测就是这个数）。
  //       而上一次跑 27 分钟时读到 27min，是因为心跳恰好在 27 分那一格落过盘。
  //   ⇒ 够回答"长跑 vs 几分钟"，**不够**做秒级计时（要更细就得缩短心跳周期，
  //     代价是 NVS 写入次数，见文件头那段账）。
  uint32_t prev_up_ms = kBootUpUnknown;
  // 本次是第几次上电/复位（含这一次；NVS 读不到时从 1 起算）。
  uint32_t boot_count = 1;
  // ★★ NVS 这一层能不能用。判据是**写**，不是读（两者必须分得开，这是本文件踩过一次的坑）：
  //   · `false` ⇒ **这一次的 reason/计数没能落盘**（开机那行要标 `nvs-`）——
  //     于是"下一次开机"仍然看不到这一次 —— 这件事必须**明说**，不许假装写成功了；
  //   · `true` + `prev_valid == false` ⇒ **这是第一次**（全新板子 / 第一次跑带本层的固件）：
  //     NVS 是好用的，只是里面还没有"上一次"。★ 这两种情况在日志里长得完全不同
  //     （`prev=-` vs `nvs-`），**不要**把它们合并成一格。
  //   ★ 为什么不能用"一个键都读不到"当坏 NVS：全新板子**就是**一个键都没有。
  bool     nvs_ok = false;
};

// ---- 守护四个计数的快照（本次 vs 累计）----
struct GuardTotals {
  uint32_t rd = 0, fix = 0, bl = 0, anom = 0;
};

class BootPersist {
 public:
  // 注入。`read`/`write` 必须给（本层的全部意义就是它俩）；字符串那两个允许为 nullptr
  // ⇒ 那就**不存/不读** `prev=` 的名字（数值仍然在），设备侧两个都给。
  BootPersist(BootNvsReadFn read, BootNvsWriteFn write,
              BootNvsReadStrFn read_str, BootNvsWriteStrFn write_str, void* ctx);

  // ★★ 开机一次性调用：**读回上一次 → 记下本次 → 打点**。
  //   顺序是**有意的**（每一步都对着一个"掉电时刻"）：
  //     ① 先把 NVS 里"上一次"的四个值读出来（心跳 uptime / reason / 名字 / 启动次数）；
  //     ② 把**本次**的 reason 与当前 uptime 写进去 —— 这一次写**不能省**：
  //        它保证"哪怕这一次开机之后再也没走到心跳，下一次开机也知道这一次为什么起来"；
  //        同时把 `prev_up` 更新成"这一次开机时已经跑了 0ms"⇒ 下一次算出来的是**本次**时长；
  //     ③ 启动次数 +1 并写回（`n` 比"已经发生过的重启次数"大 1 的那套口径见 §13.8.6）；
  //     ④ 把"上一次运行了多久"算出来（见 `BootInfo::prev_up_ms`）。
  //   ★ 返回值是 `BootInfo`（含 `nvs_ok`），调用方据此打那一行 —— 本函数**自己不打印**。
  BootInfo begin(uint32_t now_ms, uint32_t reason_raw, const char* reason_name);

  // ---- 只读读数 ----
  const BootInfo& boot() const { return boot_; }
  // 守护的**累计**基数（开机时从 NVS 读回来的、"上一次开机为止"的累计值）。
  const GuardTotals& guardBase() const { return base_; }
  // 累计值落盘过几次（跨重启单调 +1）。★ 它是"累计那一格新不新"的判据：
  //   `snapshots` 变了 ⇒ 从那一刻起累计值包含了上一次运行的尾巴。
  uint32_t snapshots() const { return snaps_; }
  // 心跳**累计**写过几次（NVS 里那个计数，跨重启也涨）。★ 它是"心跳到底有没有在跑"
  // 的判据：开机 20 分钟之后它应该 ≥ 2（这一次开机的头 10 分钟 + 第二个 10 分钟）。
  uint32_t heartbeats() const { return hb_n_; }
  uint32_t lastHeartbeatMs() const { return hb_last_ms_; }   // 最近一次心跳时的 uptime
  bool     nvsOk() const { return boot_.nvs_ok; }

  // ★★ 主循环每轮调一次。**只做两件事**：判"到没到 10 分钟"（整数比较），
  //   到点才写 NVS。★ 它**不阻塞、不读 I2C、不碰显示时间线**。
  //   返回 true = 这一拍真的落盘了一次心跳（调用方据此打一行日志/计数）。
  bool tick(uint32_t now_ms, const GuardTotals& cur);

  // 强制把"当前 uptime + 守护累计"落盘一次（用于**即将重启**的路径，例如以后要做的
  // "主动重启前先留档"）。★ 它**不**改心跳的节拍（下一次心跳照旧按 10 分钟走）。
  bool flushNow(uint32_t now_ms, const GuardTotals& cur);

 private:
  // 把当前 uptime 与守护累计写进 NVS。`reason` 相关的那三项**不在这里**（见 begin）。
  bool writeSnapshot(uint32_t now_ms, const GuardTotals& cur);
  bool writeU32(BootKey k, uint32_t v);
  uint32_t readU32(BootKey k, uint32_t dflt);

  BootNvsReadFn     read_ = nullptr;
  BootNvsWriteFn    write_ = nullptr;
  BootNvsReadStrFn  read_str_ = nullptr;
  BootNvsWriteStrFn write_str_ = nullptr;
  void*             ctx_ = nullptr;

  BootInfo    boot_{};
  GuardTotals base_{};
  uint32_t    next_hb_ms_ = 0;     // 下一次心跳的到期时刻
  bool        started_ = false;

  uint32_t    hb_last_ms_ = 0;     // 最近一次心跳时的 uptime（= 落盘的那个数）
  uint32_t    hb_n_ = 0;           // 心跳次数
  uint32_t    snaps_ = 0;          // 累计值落盘次数
};

// ------------------------------------------------------------
// 五、纯函数（判据就在这里，用例直接调；不带任何状态）
// ------------------------------------------------------------

// 该不该写心跳？★ 到点才写 —— **不是**每个 tick 都写（那会把 NVS 写穿）。
// ★ 用"有符号差"判到期（`(int32_t)(now - next) >= 0`）：`millis()` 在 49.7 天回绕，
//   而这个模块要跨"上一次"与"这一次"两次开机 ⇒ 回绕必须能算对（与仓库里其它
//   时间差判据同一条写法）。
inline bool boot_heartbeat_due(uint32_t now_ms, uint32_t next_ms) {
  return (int32_t)(now_ms - next_ms) >= 0;
}

// 下一次心跳的到期时刻（饱和加法：`millis()` 回绕/极端值下不产生"永远不到期"）。
inline uint32_t boot_next_heartbeat(uint32_t now_ms, uint32_t period_ms) {
  const uint32_t next = now_ms + period_ms;
  if (next < now_ms) return UINT32_MAX;    // 溢出 ⇒ 饱和（下一次到期在一次回绕之后）
  return next;
}

// 累计计数相加（饱和到 UINT32_MAX：累计值只增不减，宁可停在最大值也不回绕成 0）。
inline uint32_t boot_accum(uint32_t base, uint32_t cur) {
  const uint32_t s = base + cur;
  if (s < base) return UINT32_MAX;
  return s;
}

// 毫秒 → 分钟（**向下取整**：报出来的是"至少跑了这么久"）。
// ★ 为什么向下取整而不是四舍五入：这一格是"事后定性"用的（长跑 vs 几分钟），
//   向下取整让 `prev_up=9min` 一定意味着"不到 10 分钟"，与心跳周期这条账对得上。
inline uint32_t boot_minutes(uint32_t ms) { return ms / 60000u; }

// "上一次运行了多久"的算法（**纯函数，边界都在这里**）：
//   · 两个来源：最后一次心跳记下的 uptime、上一次开机落盘的 uptime；
//   · 取**更大**的那个（见 `BootInfo::prev_up_ms` 的说明）；
//   · 两个都没有 ⇒ `kBootUpUnknown`（**不是 0**：0 会被读成"上次跑了一瞬间"）。
inline uint32_t boot_prev_up_ms(uint32_t hb_last_ms, bool hb_have,
                                uint32_t boot_up_ms, bool boot_have) {
  uint32_t r = kBootUpUnknown;
  if (hb_have && hb_last_ms != kBootUpUnknown) r = hb_last_ms;
  if (boot_have && boot_up_ms != kBootUpUnknown) {
    if (r == kBootUpUnknown || boot_up_ms > r) r = boot_up_ms;
  }
  return r;
}

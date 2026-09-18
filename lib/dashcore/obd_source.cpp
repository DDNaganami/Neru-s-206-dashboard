#include "obd_source.h"
#include "obd_protocol.h"

static const uint32_t kStepTimeoutMs  = 300;  // 每步 AT 命令等待
// 两次请求之间的间隔。★ 2026-09-18 从 200ms 收到 80ms:
//   ELM327 在 38400 波特率上一条请求才 5 字节(≈1.3ms),响应十几字节;
//   真正的瓶颈是 ECU 在 ISO 9141-2 上的响应时间(通常 20~50ms)。
//   80ms 是"足够保守"的值:调太紧会表现为丢响应→走 250ms 超时→刷新率不升反降,
//   所以这个数要和 SRC-Hz 一起看(见 obd_source.h)。
static const uint32_t kPollIntervalMs = 80;
static const uint32_t kWaitTimeoutMs  = 250;  // 请求发出后的响应等待(没响应才走这条)
static const char* const kInitCmds[]  = {"ATZ", "ATE0", "ATL0", "ATH0"};

// 快路:每轮都问。0x0C 转速最重要,放第一格。
static const uint8_t kFastBasePids[] = {0x0C};
static const uint8_t kFastBaseCount = 1;
// 慢路:每 kSlowEveryNTurns 轮问一次。两个都是慢变量(温度),
// 用户 2026-09-18 确认"不需要太快的更新"。
static const uint8_t kSlowPids[] = {0x05, 0x0F};
static const uint8_t kSlowCount = 2;

// -DOBD_SPEED_PID=0 强制关掉车速那一路(对比实验用:量"加/不加"两种刷新率)。
// 默认(不定义或定义为 1)= 由 0100 位图决定。
#if defined(OBD_SPEED_PID) && (OBD_SPEED_PID == 0)
#define OBD_SPEED_PID_ALLOWED 0
#else
#define OBD_SPEED_PID_ALLOWED 1
#endif

// 值域钳制:总线噪声/坏帧不会把 UI 打飞
static const float kRpmMaxValid = 9000.0f;
static const float kTempMinC    = -40.0f;
static const float kTempMaxC    = 215.0f;
// 车速不需要钳制:010D 是单字节,最大 255 —— 而 255 km/h 对这台车不可能,
// 真出现了也该如实显示(而不是悄悄改成 0 或者 300)。

void ObdSource::begin() {
  if (!s_) return;
  phase_ = Phase::Init;
  init_step_ = 0;
  support_sent_ = false;
  support_known_ = false;
  support_mask_ = 0;
  speed_supported_ = false;
  speed_polled_ = false;
  buildPollTable(false);   // 位图没回来之前:只跑快路的转速,慢路照旧每周期一次
  sendCmd(kInitCmds[0]);
  last_activity_ms_ = millis();
}

// 生成一个周期的请求序列(见 obd_source.h 头部那张图):
//   每轮:把快路各问一遍;周期里前 slow_count 轮的末尾各插一个慢路。
// 例(快路 2 路、N=4、慢路 2 个):0C 0D 05 | 0C 0D 0F | 0C 0D | 0C 0D
void ObdSource::buildPollTable(bool with_speed) {
  uint8_t fast[2] = {kFastBasePids[0], 0};
  uint8_t fast_count = kFastBaseCount;
#if OBD_SPEED_PID_ALLOWED
  if (with_speed && fast_count < 2) fast[fast_count++] = 0x0D;
#else
  (void)with_speed;
#endif

  schedule_count_ = 0;
  for (uint8_t round = 0; round < kSlowEveryNTurns; ++round) {
    for (uint8_t f = 0; f < fast_count; ++f) {
      if (schedule_count_ < kScheduleMax) schedule_[schedule_count_++] = fast[f];
    }
    // 慢路摊在整个周期里(每个周期各问一次),而不是挤在一轮里
    if (round < kSlowCount && schedule_count_ < kScheduleMax) {
      schedule_[schedule_count_++] = kSlowPids[round];
    }
  }
  speed_polled_ = false;
  for (uint8_t i = 0; i < schedule_count_; ++i) {
    if (schedule_[i] == 0x0D) speed_polled_ = true;
  }
  slot_ = 0;
}

void ObdSource::sendCmd(const char* cmd) {
  s_->print(cmd);
  s_->print('\r');
}

void ObdSource::sendRequest(uint8_t pid) {
  s_->print("01");
  if (pid < 0x10) s_->print('0');
  s_->print(pid, HEX);
  s_->print('\r');
}

void ObdSource::tick(uint32_t now_ms) {
  if (!s_) return;
  now_ms_ = now_ms;
  drain();

  // 刷新率统计:与状态机无关,单纯"这一秒收到了几个有效值"
  rpm_rate_.tick(now_ms);
  coolant_rate_.tick(now_ms);
  intake_rate_.tick(now_ms);
  speed_rate_.tick(now_ms);

  switch (phase_) {
    case Phase::Init:
      if (now_ms - last_activity_ms_ < kStepTimeoutMs) break;
      if (++init_step_ < 4) {
        sendCmd(kInitCmds[init_step_]);
        last_activity_ms_ = now_ms;
      } else {
        // AT 序列走完 → 问一次支持的 PID 位图(见 obd_source.h 的说明)
        phase_ = Phase::QuerySupport;
        support_sent_ = false;
      }
      break;

    case Phase::QuerySupport:
      if (!support_sent_) {
        sendRequest(0x00);
        support_sent_ = true;
        last_activity_ms_ = now_ms;
        break;
      }
      // 拿到位图(onSupportedPids 里已经把表建好了)或等超时。
      // ★ 拿不到位图**不能卡住整条链**:按**默认节奏**继续跑
      //   (快路只有转速,慢路两条照旧;车速那一路不开),不猜、不试。
      if (support_known_) {
        phase_ = Phase::PollIdle;
        last_activity_ms_ = now_ms;
      } else if (now_ms - last_activity_ms_ >= kWaitTimeoutMs) {
        buildPollTable(false);
        phase_ = Phase::PollIdle;
        last_activity_ms_ = now_ms;
      }
      break;

    case Phase::PollIdle:
      if (now_ms - last_activity_ms_ >= kPollIntervalMs) {
        waiting_pid_ = schedule_[slot_];
        sendRequest(waiting_pid_);
        slot_ = (uint8_t)((slot_ + 1) % schedule_count_);
        phase_ = Phase::PollWait;
        last_activity_ms_ = now_ms;
      }
      break;

    case Phase::PollWait:
      // ★ 提前离开等待态:响应已经在 onPidValue 里认领过了(见那里的说明),
      //   只有"没响应"才走到这个超时。
      if (now_ms - last_activity_ms_ >= kWaitTimeoutMs) {
        phase_ = Phase::PollIdle;
        last_activity_ms_ = now_ms;
      }
      break;
  }
}

void ObdSource::drain() {
  while (s_->available()) {
    const char c = (char)s_->read();
    if (c == '\r' || c == '\n') {
      if (buf_len_) {
        buf_[buf_len_] = '\0';
        parseLine();
        buf_len_ = 0;
      }
    } else if (c >= 32 && buf_len_ < sizeof(buf_) - 1) {
      buf_[buf_len_++] = toupper(c);
    }
  }
}

void ObdSource::parseLine() {
  uint8_t pid = 0;
  uint16_t raw = 0;
  if (parseObdLine(buf_, &pid, &raw)) {
    onPidValue(pid, raw);
    return;
  }
  // 不是数据帧 —— 也可能正是我们要的 0100 位图(它不算"数据")
  uint32_t mask = 0;
  if (parseSupportedPids(buf_, &mask)) onSupportedPids(mask);
}

// 0100 位图回来了:按 ECU 自己说的决定车速那一路要不要占时隙。
// 只认第一次 —— 后面的重复响应(或别的服务里冒出来的 4100)不覆盖。
void ObdSource::onSupportedPids(uint32_t mask) {
  if (support_known_) return;
  support_mask_ = mask;
  support_known_ = true;
  speed_supported_ = pidSupported(mask, 0x0D);
  buildPollTable(speed_supported_);
}

void ObdSource::onPidValue(uint8_t pid, uint16_t raw) {
  // ★ 先认领响应:如果这一格问的正是它,就别再等满 kWaitTimeoutMs 了。
  //   不认领会怎样(2026-09-18 实测的旧行为):每个请求固定花掉
  //   250(等)+80(间隔)=330ms,不管 ECU 其实 30ms 就答完了 ——
  //   白白扔掉三分之二的时间,K 线那条窄管子被浪费掉大半。
  //   只认"当次请求的" PID:上一轮的残留响应(或别的模块的帧)不能算数。
  if (phase_ == Phase::PollWait && pid == waiting_pid_) {
    phase_ = Phase::PollIdle;
    last_activity_ms_ = now_ms_;
  }

  if (pid == 0x0C) {
    const float r = rpmFromRaw(raw);
    if (r < 0.0f || r > kRpmMaxValid) return;  // 坏帧丢弃
    rpm_ = r;
    rpm_valid_ = true;
    last_rpm_ms_ = now_ms_;
    rpm_rate_.count++;
  } else if (pid == 0x05) {
    const float c = coolantFromRaw(raw);
    if (c < kTempMinC || c > kTempMaxC) return;  // 坏帧丢弃
    coolant_ = c;
    coolant_valid_ = true;
    last_coolant_ms_ = now_ms_;
    coolant_rate_.count++;
  } else if (pid == 0x0F) {
    // 010F 进气温度:A-40,与 0105 同一个换算(所以共用 intakeFromRaw)。
    // ★ ECU 不支持这一项时,ELM327 回的是 "NO DATA" —— parseObdLine 解不出
    //   PID,根本走不到这里,于是 hasIntake() 一直是 false,上层按字段回退。
    const float t = intakeFromRaw(raw);
    if (t < kTempMinC || t > kTempMaxC) return;  // 坏帧丢弃
    intake_ = t;
    intake_valid_ = true;
    last_intake_ms_ = now_ms_;
    intake_rate_.count++;
  } else if (pid == 0x0D) {
    // 010D 车速:单字节 km/h。只有 0100 位图说支持、而且没被 -DOBD_SPEED_PID=0
    // 关掉时才会走到这里(不然没人发这个请求,自然也没有响应)。
    speed_ = speedFromRaw(raw);
    speed_valid_ = true;
    last_speed_ms_ = now_ms_;
    speed_rate_.count++;
  }
}

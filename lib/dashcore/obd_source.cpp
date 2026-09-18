#include "obd_source.h"
#include "obd_protocol.h"

static const uint32_t kStepTimeoutMs  = 300;  // 每步 AT 命令等待
static const uint32_t kPollIntervalMs = 200;  // 两次 PID 请求间隔
static const uint32_t kWaitTimeoutMs  = 250;  // 请求发出后的响应等待
static const char* const kInitCmds[]  = {"ATZ", "ATE0", "ATL0", "ATH0"};

// 基准轮询表:三轮一转。0x0C 转速最重要,放第一格。
// 车速(0x0D)不在这里 —— 它是**条件**加入的(见 buildPollTable)。
static const uint8_t kBasePollPids[] = {0x0C, 0x05, 0x0F};
static const uint8_t kBasePollCount = 3;

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
  poll_count_ = kBasePollCount;
  for (uint8_t i = 0; i < kBasePollCount; ++i) poll_pids_[i] = kBasePollPids[i];
  pid_slot_ = 0;
  sendCmd(kInitCmds[0]);
  last_activity_ms_ = millis();
}

void ObdSource::buildPollTable(bool with_speed) {
  poll_count_ = 0;
  for (uint8_t i = 0; i < kBasePollCount; ++i) {
    poll_pids_[poll_count_++] = kBasePollPids[i];
  }
#if OBD_SPEED_PID_ALLOWED
  if (with_speed && poll_count_ < sizeof(poll_pids_)) poll_pids_[poll_count_++] = 0x0D;
#else
  (void)with_speed;
#endif
  speed_polled_ = false;
  for (uint8_t i = 0; i < poll_count_; ++i) {
    if (poll_pids_[i] == 0x0D) speed_polled_ = true;
  }
  pid_slot_ = 0;
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
      // ★ 拿不到位图**不能卡住整条链**:按默认三路继续跑,不猜、不试。
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
        sendRequest(poll_pids_[pid_slot_]);
        pid_slot_ = (uint8_t)((pid_slot_ + 1) % poll_count_);
        phase_ = Phase::PollWait;
        last_activity_ms_ = now_ms;
      }
      break;

    case Phase::PollWait:
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

#include "obd_source.h"
#include "obd_protocol.h"

static const uint32_t kStepTimeoutMs  = 300;  // 每步 AT 命令等待
static const uint32_t kPollIntervalMs = 200;  // 两次 PID 请求间隔
static const uint32_t kWaitTimeoutMs  = 250;  // 请求发出后的响应等待
static const char* const kInitCmds[]  = {"ATZ", "ATE0", "ATL0", "ATH0"};

// 值域钳制:总线噪声/坏帧不会把 UI 打飞
static const float kRpmMaxValid = 9000.0f;
static const float kCoolantMinC = -40.0f;
static const float kCoolantMaxC = 215.0f;

void ObdSource::begin() {
  if (!s_) return;
  phase_ = Phase::Init;
  init_step_ = 0;
  sendCmd(kInitCmds[0]);
  last_activity_ms_ = millis();
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

  switch (phase_) {
    case Phase::Init:
      if (now_ms - last_activity_ms_ < kStepTimeoutMs) break;
      if (++init_step_ < 4) {
        sendCmd(kInitCmds[init_step_]);
      } else {
        phase_ = Phase::PollIdle;
      }
      last_activity_ms_ = now_ms;
      break;

    case Phase::PollIdle:
      if (now_ms - last_activity_ms_ >= kPollIntervalMs) {
        sendRequest(next_pid_);
        next_pid_ = (next_pid_ == 0x0C) ? 0x05 : 0x0C;
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
  if (parseObdLine(buf_, &pid, &raw)) onPidValue(pid, raw);
}

void ObdSource::onPidValue(uint8_t pid, uint16_t raw) {
  if (pid == 0x0C) {
    const float r = rpmFromRaw(raw);
    if (r < 0.0f || r > kRpmMaxValid) return;  // 坏帧丢弃
    rpm_ = r;
    rpm_valid_ = true;
    last_rpm_ms_ = now_ms_;
  } else if (pid == 0x05) {
    const float c = coolantFromRaw(raw);
    if (c < kCoolantMinC || c > kCoolantMaxC) return;  // 坏帧丢弃
    coolant_ = c;
    coolant_valid_ = true;
    last_coolant_ms_ = now_ms_;
  }
}
